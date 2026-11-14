#!/usr/bin/env python3
"""Generate src/offsets_checks.inc: one runtime check per hard-coded game offset.

Every plain RVA / field constant in src/offsets.h gets a check that the mod runs
once per module at startup (src/offset_checks.cpp). A failed check lands in the
"VR MOD OUT OF DATE" toast with the constant's name, so a Black Mesa update that
moves something shows up at launch instead of in a specific scene.

Check kinds (all O(1) at runtime, no image scans):
  CODE   masked bytes at the address (function start: [rva, rva+32);
         mid-function / return address: [rva-16, rva+16)). Relocated dwords and
         branch targets are wildcards.
  DATA   a game instruction that references the global (relocated dword at
         site+ref == module base + constant) plus masked bytes around it.
  FIELD  the game's own registration of the field the constant's comment names:
         `push <offset>; push "m_name"` (datamap) in .text, or a RecvProp
         {char* name; int offset} pair in .data.
Field constants whose comment names no registered field, vtable slots and
other small numbers are listed as unchecked (printed, not emitted).

The module of each RVA constant is in MODULES below: a new RVA constant must be
added there (the script stops otherwise). Run against the installed game:

    python3 -I tools/offsets/gen_offset_checks.py [--game "<Black Mesa dir>"]

Regenerate after every port (tools/offsets/port_offsets.py) and whenever an
offset constant is added or changed; the runtime check of a constant whose
value was edited without regenerating fails, which is the point.
"""
import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE, relocs, masked, search  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DEFAULT_GAME = os.path.expanduser('~/.local/share/Steam/steamapps/common/Black Mesa')
DLL = {'client': 'bms/bin/client.dll', 'server': 'bms/bin/server.dll', 'engine': 'bin/engine.dll',
       'materialsystem': 'bin/materialsystem.dll', 'shaderapidx9': 'bin/shaderapidx9.dll'}

# Module of every RVA constant in offsets.h (value >= 0x10000).
MODULES = {
    'client': '''kClientModeGlobalRva kCBaseEntity_SetAbsOrigin kCBaseEntity_SetAbsAngles kCBaseEntity_EmitSound
        kCBasePlayer_Weapon_ShootPosition kC_BlackMesaPlayer_GetShootAngles kCBaseAnimating_GetAttachmentVec
        kCBaseAnimating_GetAttachmentMatrix kFormatViewModelAttachment kCTauBeam_ViewMuzzle kCTauBeam_WorldBeam
        kCTauBeam_FireTrace kCWeaponGluon_ImpactTrace kCWeaponGluon_BeamUpdate kCGluonBeamFx_SetBeam
        kCGluonBeamFx_Draw kParticleSetControlPoint kParticleMgr_AddEffect kParticleSystemMgr
        kParticleSystemMgr_Find kClientGpGlobalsPtr kCWeaponRpg_UpdateLaser kCHudCrosshair_Paint
        kCSpriteRenderer_DrawSprite kCSpriteRenderable_Draw kCEnvLaserDot_Draw kSpriteQuad kGetSpriteAxes
        kCurrentViewOriginGlobal kCViewRenderBeams_DrawBeam kClientCurrentViewIdRva kPixelVisQueryAgeCmpRva
        kPixelVisSetAgeCmpRva kPixelVisQueryFractionRva kSaveShotWriterRva kSaveShotNowRva
        kPixelVisGetFractionRva kPixelVisQueryForViewRva kClientFrameStartRva kClientSetupBonesRva
        kClientBuildTransformationsRva kClientParticleCollectionSimulateRva kClientParticleRenderStartRva kClientDrawTranslucentARva kClientDrawTranslucentBRva
        kClientPrecipRenderRva kClientPcfCameraFacingRva kClientParticleMgrSimulateRva
        kLensflareWorldToScreenRva kClientFrameFrustumRva kGeneratePerspectiveFrustumRva
        kLensflareFractionRva kLensflareDrawQuadRva kLensflareElementRva kClientMaterialsPtrRva
        kUpdateFlashlightState kBmFlashlightUpdate kEyeVectors kFogColorEyeVectorsCall
        kSkyFogColorEyeVectorsCall kPixelVisViewIdCallRvas''',
    'server': '''kCBasePlayer_Weapon_ShootPosition_Server kCBasePlayer_GrabHoldOrigin_Server
        kBlackMesaPlayer_ShootSource_Server kCGrabController_SetTargetPosition
        kBlackMesaPlayer_GetShootAngles_Server kGrenadeCreate_Server kGrenadeThrowPosition_Server
        kProjectileCreate_Server kCBasePlayer_ImpulseCommands kServerWeaponFireOriginReturnRvas''',
    'engine': '''kAreaPortalComputeViewReturnRva kEngineWorldBatchRva kEngineWorldStaticMeshesRva
        kMixPlayInsnRva''',
    'materialsystem': 'kCreateNamedRTExRva',
}
MODULE_OF = {n: m for m, names in MODULES.items() for n in names.split()}
# Mid-function addresses (an instruction or a return address, not a function start).
MID = re.compile(r'(Return|Insn|Cmp|Call)')


def constants(text):
    """[(name, index or None, value, comment block)] for hex constants in offsets.h."""
    out = []
    lines = text.splitlines()
    comment = []
    for i, line in enumerate(lines):
        s = line.strip()
        if s.startswith('//'):
            comment.append(s)
            continue
        m = re.match(r'static constexpr \w+ (k\w+)(\[\d*\])? = (\{[^}]*\}|0x[0-9A-Fa-f]+|\d+);(.*)', s)
        if not m:
            if s and not s.startswith('Offset ') and not s.startswith('"'):
                comment = []
            continue
        name, arr, val, tail = m.groups()
        if arr is not None:
            j = i
            body = line
            while '}' not in body:
                j += 1
                body += lines[j]
            vals = re.findall(r'0x[0-9A-Fa-f]+', body.split('{', 1)[1].split('}', 1)[0])
            for k, v in enumerate(vals):
                out.append((name, k, int(v, 16), ' '.join(comment) + tail))
        else:
            out.append((name, None, int(val, 0), ' '.join(comment) + tail))
        comment = []
    return out


def hexpat(raw, mask):
    return ' '.join('%02X' % b if m else '??' for b, m in zip(raw, mask))


def u32(pe, rva):
    return struct.unpack_from('<I', pe.d, pe.rva2off(rva))[0]


def code_check(pe, rva, mid):
    """Smallest window (32..96 bytes) whose masked bytes occur once in .text. Identical
    thunks never get unique: then the 96-byte window is used and the count returned."""
    start = -16 if mid else 0
    for n in (32, 48, 64, 96):
        raw, mask = masked(pe, rva + start, n)
        hits = search(pe, raw, mask)
        if len(hits) == 1:
            break
    return start, raw, mask, len(hits)


def data_check(pe, rva):
    """A .text reloc site referencing base+rva whose masked window is unique."""
    t = pe.text()
    for r in relocs(pe):
        if not (t[1] <= r < t[1] + t[2]) or u32(pe, r) != pe.base + rva:
            continue
        raw, mask = masked(pe, r - 8, 24)
        if len(search(pe, raw, mask)) == 1:
            return r - 8, 8, raw, mask
    return None


def field_sites(pe, name):
    """[(kind, site, value)] registrations of field `name`: datamap pushes and RecvProp pairs."""
    out = []
    key = name.encode() + b'\0'
    rd = [x for x in pe.secs if x[0] in ('.rdata', '.data')]
    addrs = []
    for sec in rd:
        n, va, vs, ra, rs = sec
        blob = pe.d[ra:ra + rs]
        for m in re.finditer(re.escape(key), blob):
            if m.start() == 0 or blob[m.start() - 1] == 0:
                addrs.append(va + m.start())
    for a in addrs:
        needle = struct.pack('<I', pe.base + a)
        n, va, vs, ra, rs = pe.text()
        blob = pe.d[ra:ra + rs]
        for m in re.finditer(re.escape(b'\x68' + needle), blob):
            p = m.start()
            if p >= 5 and blob[p - 5] == 0x68:
                out.append(('PUSH', va + p - 5, struct.unpack_from('<I', blob, p - 4)[0], a))
        for sec in [x for x in pe.secs if x[0] == '.data']:
            n, va, vs, ra, rs = sec
            blob = pe.d[ra:ra + rs]
            for m in re.finditer(re.escape(needle), blob):
                if m.start() % 4 == 0 and m.start() + 8 <= len(blob):
                    out.append(('RECV', va + m.start(), struct.unpack_from('<I', blob, m.start() + 4)[0], a))
    return out


def fingerprint(pe):
    e = struct.unpack_from('<I', pe.d, 0x3C)[0]
    ts = struct.unpack_from('<I', pe.d, e + 8)[0]
    size = struct.unpack_from('<I', pe.d, e + 24 + 56)[0]
    return ts, size


def build_id(game):
    acf = os.path.join(game, '..', '..', 'appmanifest_362890.acf')
    try:
        m = re.search(r'"buildid"\s+"(\d+)"', open(acf).read())
        return m.group(1) if m else '?'
    except OSError:
        return '?'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default=DEFAULT_GAME)
    ap.add_argument('--out', default=os.path.join(REPO, 'src', 'offsets_checks.inc'))
    args = ap.parse_args()
    pes = {m: PE(os.path.join(args.game, p)) for m, p in DLL.items()}
    text = open(os.path.join(REPO, 'src', 'offsets.h')).read()
    entries, unchecked, errors, warnings = [], [], [], []
    for name, idx, val, comment in constants(text):
        ref = f'Offsets::{name}' + (f'[{idx}]' if idx is not None else '')
        label = name + (f'[{idx}]' if idx is not None else '')
        if val >= 0x10000:
            mod = MODULE_OF.get(name)
            if not mod:
                errors.append(f'{name}: no module in MODULES')
                continue
            pe = pes[mod]
            t = pe.text()
            if t[1] <= val < t[1] + t[2]:
                start, raw, mask, hits = code_check(pe, val, bool(MID.search(name)) or idx is not None)
                if hits == 0:
                    errors.append(f'{label}: code window does not match in {mod}')
                    continue
                if hits > 1:
                    warnings.append(f'{label}: {hits} identical copies in {mod} (bytes checked, location not unique)')
                entries.append(f'    {{ "{mod}.dll", "{label}", OffsetCheckKind::Code, {ref}, {start}, 0, '
                               f'"{hexpat(raw, mask)}", nullptr }},')
            else:
                d = data_check(pe, val)
                if not d:
                    errors.append(f'{label}: no unique code reference to the global in {mod}')
                    continue
                start_rva, refoff, raw, mask = d
                entries.append(f'    {{ "{mod}.dll", "{label}", OffsetCheckKind::Data, {ref}, {start_rva:#x}, {refoff}, '
                               f'"{hexpat(raw, mask)}", nullptr }},')
            continue
        fields = sorted(set(re.findall(r'\b(m_[A-Za-z0-9_]+)', comment)))
        done = False
        for f in fields:
            for mod in ('client', 'server'):
                if mod == 'client' and name.endswith('_Server'):
                    continue
                if mod == 'server' and not name.endswith('_Server'):
                    continue
                sites = [s for s in field_sites(pes[mod], f) if s[2] == val]
                if not sites:
                    continue
                kind, site, _, _ = sites[0]
                k = 'FieldPush' if kind == 'PUSH' else 'FieldRecv'
                entries.append(f'    {{ "{mod}.dll", "{label}", OffsetCheckKind::{k}, {ref}, {site:#x}, 0, nullptr, "{f}" }},')
                done = True
                break
            if done:
                break
        if not done:
            unchecked.append(f'{label} = {val:#x}' + (f' (comment names {", ".join(fields)}, none registered at that offset)' if fields else ''))
    if errors:
        print('ERRORS (nothing written):', *errors, sep='\n  ')
        sys.exit(1)
    fp = []
    for m in DLL:
        ts, size = fingerprint(pes[m])
        fp.append(f'    {{ "{m}.dll", {ts:#010x}, {size:#x} }},')
    bid = build_id(args.game)
    with open(args.out, 'w', newline='\n') as f:
        f.write('// Generated by tools/offsets/gen_offset_checks.py - do not edit by hand.\n')
        f.write(f'// Black Mesa buildid {bid}. {len(entries)} checks; regenerate after a port or an offsets.h change.\n')
        f.write(f'static const char kOffsetChecksGameBuild[] = "{bid}";\n')
        f.write('static const OffsetModuleFingerprint kOffsetFingerprints[] = {\n' + '\n'.join(fp) + '\n};\n')
        f.write('static const OffsetCheck kOffsetChecks[] = {\n' + '\n'.join(entries) + '\n};\n')
    print(f'wrote {args.out}: {len(entries)} checks, build {bid}')
    print(f'warnings ({len(warnings)}):', *warnings, sep='\n  ')
    print(f'unchecked ({len(unchecked)}):', *unchecked, sep='\n  ')


if __name__ == '__main__':
    main()
