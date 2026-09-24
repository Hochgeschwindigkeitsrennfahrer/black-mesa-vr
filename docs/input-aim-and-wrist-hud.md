# Motion input, aim geometry, and the wrist HUD

Status: §1 is **runtime-verified** (swing peaks of 5–14 m/s in the log, well over
the 1.5 m/s threshold). §2–§4 are implemented and were tested in the headset, with
§2 partially corrected by §7. §5a, §6 and §7 are implemented and compiled but
**not yet runtime-verified.**

## 1. OpenXR controller velocity was zero (crowbar melee regression)

`UpdateCrowbarMelee` gates on `m_RightControllerSpeedMs`, which comes from
`TrackedDevicePose_t::vVelocity`. OpenXR poses carry no velocity, so
`ConsumeOpenXrTracking` differences it from position.

The bug: `ConsumeOpenXrTracking` runs **several times per rendered frame** —
`WaitPosesForStereoFrame`, `BeginStereoFramePose`, and `VR::Update` — and it had
no guard on the bridge pose generation. The repeat calls re-differenced the
*same* bridge sample, so:

- position delta was exactly 0, and
- the elapsed time was sub-millisecond, below the `dtSeconds` floor,

which left `vVelocity` at its zero-initialised value. `UpdateTracking` then
computed `m_RightControllerSpeedMs = 0`, and because `m_PosesWaitedThisFrame`
suppresses the later `UpdateTracking`, that zero was what `ProcessInput` saw.
Swinging could never cross the threshold.

Fix: cache the differenced velocity and only recompute when the bridge has
published a new sample. Head and hand samples carry **independent generations**
(`L4D2VR_ReadOpenXrHmdPose` vs `L4D2VR_ReadOpenXrInputState`), so each is keyed
on its own generation and its own timestamp. Keying both off one counter would
re-difference unchanged hand positions whenever only the head pose advanced,
which reintroduces the same zero.

The swing threshold was also lowered from 2.4 m/s back to 1.5 m/s. 2.4 was
chosen while velocity was stuck at zero, so it was never actually calibrated
against real numbers. The hysteresis off-threshold and the 400 ms cooldown are
what stop `IN_ATTACK` machine-gunning, not the on-threshold. A rate-limited
"peak speed" line is logged every 5 s while the crowbar is out so a swing that
never crosses the threshold can be told apart from velocity being dead again.

## 2. Shots did not follow the controller aim ray

Bullets travel along `cmd->viewangles`, which `dCreateMove` sets from
`GetRightControllerAbsAngle()`. The *origin* was rewritten by
`Weapon_ShootPosition` to `TryGetVrMuzzleWorld()`.

Those are two different rays. The muzzle sits off the controller axis by the
per-weapon viewmodel position offset, and `ApplyViewmodelVisualScale` then
scales it about the controller pivot. So every weapon put its shots a fixed
distance off target, and the **revolver was worst because it is the only weapon
scaled up** (`s *= 1.15f`), pushing the muzzle furthest from the pivot.

Fix: `TryGetVrShootOrigin` projects the muzzle onto the controller aim ray,
keeping its distance along the barrel. The shot now leaves from a point on the
ray the controller points down, regardless of how the model is posed or scaled.
`TryGetVrMuzzleWorld` is unchanged and still describes the visual muzzle.

## 3. Aim fell back to the headset during controller dropouts

`applyControllerAim` used the HMD angle whenever `m_ControllerPoseValid` was
false. The player is normally looking well above where the gun is pointed, so
shots went high — and it stuck for as long as the pose stayed invalid, which
matches "shoots way above where you're aiming until you recenter". It now holds
the last good controller aim across dropouts, and only uses the headset before
any controller aim has been seen.

## 4. Two-hand weapons

The old shotgun-only path used a fake 0.28 m forend, a 55 % blend, and a 50°
clamp. The barrel lagged the off-hand, then followed it to the hip.

`ApplyTwoHandWeaponAim` runs on long guns, the MP5, the 357, and the glock.
Grab is **left grip only** while the support hand is on the model — no
proximity auto-latch. Shotgun / crossbow / RPG / tau / gluon / hivehand use
the viewmodel `L_Wrist` / `L_Hand` bone when it is a real forend. The shotgun
pump is a rail from that bone toward the barrel, not a single rear point. MP5
ignores L_Wrist (magazine) and uses a left-side handguard table. Pistols snap
the glove immediately left of the gun-hand controller onto the grip without
rotating aim. Release when the squeeze lets go or the support hand leaves the
rail. Long-gun shots follow the two-hand barrel (`GetAimAngles`). **Not
HMD-verified.**

## 5. Wrist HUD icons

Icons are the game's own, pulled from `bms_textures_dir.vpk` through the VPK
reader / VTF decoder / texture upload path the weapon wheel already owns, now
shared via `src/vr_hud_icons.h`. Confirmed present in the VPK with
`tools/list_vpk.py`:

| Slot | Asset |
| --- | --- |
| Health | `materials/vgui/hud/hud_health_overlay.vtf` |
| Suit | `materials/vgui/hud/hud_hev_overlay.vtf` |
| Ammo | `ammo_9mm`, `ammo_357`, `ammo_buckshot`, `ammo_bolt`, `ammo_energy`, `ammo_grenade_rpg`, `ammo_grenade_frag`, `ammo_grenade_satchel`, `ammo_grenade_tripmine`, `ammo_snark` |
| MP5 secondary | `ammo_grenade_mp5.vtf` |

All are 256×256 or 512×512 DXT5, which `DecodeVtfToBgra` already handles.

Black Mesa ships **no** `ammo_grenade_hornet.vtf` (only the `.vmt`), so the
hivehand resolves to a name that is not there. `AcquireHudIcon` logs the miss
once and returns null; the counter then draws without an icon.

Layout follows the HEV HUD: a dim `000` behind each value so the unused leading
digits stay visible, the value over it, then the icon. Health turns red at or
below 25, matching `warnIfLessThan 25` in the game's own `scripts/hudlayout.res`.

Positioning is controller-local, not HL2VR player.mdl / muzzle attachments.
Those tilted the health panel into the HEV glove and sent ammo to a different
place on every gun. Health/suit sit flat on the back of the off-hand (2.5
units), shifted toward the wrist and lowered off the knuckles. Ammo is a
2-unit sideways plate beside the gun-hand controller, facing inward.

Drawing matches HL2VR VGUI screens: IgnoreZ (HUD after the glove mesh, depth
off) so the hand cannot cover it, and `IsBackfacing` so ammo is one-sided —
looking through the gun from the other side does not show mirrored digits.
Icons and row layout are unchanged.

`tools/list_vpk.py` lists any VPK's contents and is the quickest way to confirm
an asset name before referencing it from code.

### 5a. Icons rendered as solid blocks (alpha was never enabled)

First runtime test showed every icon as a filled amber square and the dim `000`
backing at full brightness. Both came from one line: `DrawIndependentHandMarkers`
sets `D3DRS_ALPHABLENDENABLE, FALSE` for the whole overlay. With blending off,
D3D9 discards the alpha channel entirely — so the icons, which are white RGB
with their shape carried in alpha (a Source HUD convention), filled their quads
solid, and the backing digits' alpha of 55 was ignored.

The log ruled out the decoder as the cause before any change was made: the
upload sizes (`hud_hev_overlay.vtf 143x249` from a 256×256 source) are
`CropBgraToAlpha` results, which are only reachable if the alpha channel decoded
correctly. `DrawHandHud` now enables `SRCALPHA/INVSRCALPHA` for its own draws and
restores `ALPHABLENDENABLE` to FALSE before returning, leaving the opaque hand
and debug-box drawing unchanged.

Two layout corrections at the same time: the backing now only fills the digit
cells the value does *not* reach, so a bright digit is never composited on top of
a dim one, and the reserve and MP5 secondary counters pass a zero-alpha backing
to opt out of the `000` field entirely.

## 6. MP5 fire walked upward: the recoil is added on the server

`ZeroPlayerViewRecoil` zeroes `m_vecPunchAngle` and `m_recoilPunchAngles` on the
**client** player every `CreateMove` and `CalcViewModelView`. Bullets kept
climbing anyway, because the client copies are not what aims the shot.

`CBlackMesaPlayer::GetShootAngles` (server.dll RVA `0x47EC90`, player vtable
`+0x240`) is the sole source of the bullet direction. All 0x8C bytes were
disassembled from the shipped DLL and the whole function is:

```
*out  = EyeAngles()              ; virtual, vtable +0x234
*out += [this+0x934 .. 0x93C]    ; m_Local.m_vecPunchAngle
*out += [this+0x14BC .. 0x14C4]  ; m_recoilPunchAngles
ret 4
```

Each weapon's attack calls `+0x240` to build the fire direction and calls
`ApplyRecoil` *after* the shot, so recoil accumulates into `m_recoilPunchAngles`
and biases the *next* shot. That is exactly "impacts walk up during sustained
fire". `scripts/gameplay/weapons/weapon_mp5.dmx` confirms the direction is always
up: `min_punch_angles -1`, `max_punch_angles -0.5` on pitch.

`dGetShootAngles` calls the original and subtracts back the `+0x14BC` term. The
client's own recoil state is untouched, so the gun still kicks on screen while
bullets follow the aim. Gated by `DisableRecoilAim` (default on).

Two things this deliberately does *not* do:

- It leaves `m_vecPunchAngle` in the sum. That is the legacy damage-flinch
  spring, not weapon recoil, and `sv_suppress_viewpunch` already suppresses it.
  That cvar is read at exactly one address — the early-return in
  `CBasePlayer::ViewPunch` — and could never have affected weapon recoil, since
  `AddRecoil` contains no cvar read at all. The comment in `ApplyVrQualityOfLifeCvars`
  claiming otherwise has been corrected.
- It does not touch bullet **spread** (`spread 1.75 1.75` for the MP5), which is
  a random cone rather than a climb.

## 7. Aim pitch trim

With the shot origin now on the aim ray (§2), the constant upward offset the old
muzzle origin contributed is gone, which exposed a residual *downward* bias that
offset had been masking. That bias is grip calibration — how far
`ControllerPitchTilt` (-35° by default) has to rotate a given controller to point
like a gun — so it has no single correct value.

`AimPitchOffset` (degrees, positive shoots higher) trims the firing pitch only,
leaving tuned viewmodel poses alone. `VR::GetAimAngles` is now the single
definition of the firing ray, read by both `cmd->viewangles` and the shot-origin
projection so the two cannot drift apart again.

Tune it live with **Ctrl+Numpad+ / Ctrl+Numpad-** (hold Shift for 0.1° steps).
Each change logs the running total; copy it into `AimPitchOffset` in
`VR/config.txt` to persist.
