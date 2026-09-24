# Current progress (2026-08-21)

Read this file at the start of every session. Do not rediscover items below.

## Working (user-verified; do not rewrite)

- Fused 3D stereo, uncoupled 6DoF, save-load (nested `LockSubmissionQueue` deadlock fixed).
- Left-menu pause activation (slot 108 engine thread).
- Desktop HUD visible (do not steal `_rt_gui` destination).
- Crosshair off via `bmvr.cfg`.
- **FP arms hidden in HMD** — bodypart `arms` `nummeshes=0` sticky patch. User confirmed gone.
- Independent left/right controller tracking in-game: **cyan left**, **magenta right**, each follows its own controller (debug boxes; superseded by GLB gloves below).
- **Weapon proportions in HMD** — view-Y unstretch; upright vs flat controller matches desktop Glock shape (user-verified 2026-08-19).
- **Walk jump/ghost of the gun** — user-verified 2026-08-19. Velocity bob-kill at RecvTable `+0xF8`; DME rigid bone snap + 4-slot ring; `cl_viewmodel_lag` / `r_jiggle_bones` off with `DisableViewBob`.
- HEV gloves work in HMD. Do **not** enable `VrHandsRightUseViewmodelPose`.

## Why 47777b5 looked identical on desktop and HMD

`47777b5` (~30–40 FPS) coupled the engine G-buffer and the HMD eyes: they were the same pixels. Multicore is not what made later builds worse on this machine: it is already off (`AutoMatQueueMode=false`, skip `mat_queue`), and L4D2VR’s own default is also queue-off. Do **not** re-enable `SetThreadMode(2)`.

The first break is the immediate child **`d24bc07`** (no commits in between). It stopped sizing FullFrame to the HMD, always crop-blit 16:9 → eyes, and skipped leftover 16:9 `RenderView`s. Later `bad197a` flashlight ImpulseCommands is **not** the original break.

| Topic | `47777b5` (known-good) | `d24bc07` (first break) | This pass (restore same-buffer) |
|---|---|---|---|
| FullFrame | LITERAL HMD-fit (~1584×1440 in 2560×1440 HWND) | Engine 16:9 window size | LITERAL HMD-fit again (`ff_hmdfit`; **not** `ff_stereo` grow) |
| GetScreenSize / BB dims | Report HMD-fit | Keep window size except during eye blit | HMD-fit for non-nested, non-aux (HUD inset kept) |
| Leftover `RenderView` | Skip **same-size** duplicates only | Also skip leftover 16:9 mains | Same-size only (`windowed169` skip reverted) |
| Eye blit | 1:1 when sizes match | Always top-left crop of 16:9 BB | 1:1 unbind when FullFrame == eyes; crop is fallback |
| Multicore | `GetMatQueueMode` stub 0 | `AutoMatQueueMode` + `SetThreadMode(2)` | Queue **off** (already skipped; do not re-enable) |
| Flashlight | Same stereo pair feeds desktop + HMD | Deferred apply stayed on leftover 16:9 | HMD `FlashlightState` retarget kept; apply should run in eye RV |

Named `leftEye0` / `steamvr_rt` / `hmd_native` / FullFrame **grow** (`ff_stereo`) stay crash-sticky. Do **not** retry those.

## Flashlight vs fused stereo (2026-08-22; do not retry)

Fused stereo (1584×1440 HMD FOV + IPD + `GetProjectionRaw` UVs, G-buffers stay 2560) is the known-good HMD path. Desktop leftover 16:9 after stereo still applies flashlight on the monitor. Eyes blit **before** that leftover, so the HMD beam is missing.

Tried and rejected:

| Attempt | Result | Retry |
|---|---|---|
| Overlay hide / BB-size lie during stereo | Flashlight still missing | no |
| Stereo at 2560 16:9 + center crop + Submit 0..1 | Flashlight works; **stretched, stereo broken** | **no** |
| `ff_hmdfit` LITERAL FullFrame 1584, GB still 2560 | White HMD (A2R10 unbind) | **no** |
| `ff_gbfit` LITERAL FullFrame **and** `_rt_gb*` 1584 | Alloc 1584; process died on `background04` before stereo; user miss | **no** |

Keep fused 1584 eyes + 2560 world RTs. Leftover 16:9 after stereo stays (desktop beam). HMD flashlight still needs a different approach — not RT resize, not 16:9 stereo.

## Pass 2026-08-22 `fl_gbmatch` (compiled; not HMD-verified)

Stereo `CViewSetup` stays **2560×1440** (G-buffer size) with **HMD fov/aspect/IPD**. Viewport is not clamped to 1584. Squash-blit the full 2560 BB into 1584 eyes. Leftover 16:9 still runs for the desktop. Do **not** resize RTs. If the headset stretches or fusion breaks, persist-skip `fl_gbmatch` — that is the 16:9 stereo failure mode. Log: `gbmatch=1`, `left RenderView 2560x1440`, `HMD BB blit … squash=1`, `Flashlight PushRT inside eye RV`.

## Pass 2026-08-21 same-buffer restore (compiled + installed; **not HMD-verified**)

- Restore `47777b5` same-buffer: CreateNamedRT `_rt_FullFrameFB*` only (not `_rt_gbDepth2`/`_rt_gbNormal2` 1024 PICMIP). Sticky `ff_hmdfit`. Never grow. **2026-08-21 HMD white textures:** log showed unbind of A2R10 FullFrame (`fmt=35`) while desktop was 2560 composite; HMD-fit had also LITERAL-resized G-buffer *2 downsamples 1024→1584 (deferred albedo broken, flashlight still applied). Do not HMD-fit PICMIP/explicit-size `_rt_gb*`.
- Leftover skip: only same-as-stereo duplicates. Unbind StretchRect is 1:1 when sizes match. `m_DesktopMirrorEnabled` stays false (A2R10 black stretch).
- Flashlight: keep `dUpdateFlashlightState` HMD origin/forward. Log `Flashlight PushRT inside eye RV` if `_rt_gbShadowMapFlashLight` happens during an eye `RenderView`.
- Hands: right glove **off** (`VrHandsRightEnabled=false`). Left stays on. Grip `Rz=-180`. Scales unchanged.
- Anims: never rewrite `m_nSequence`. Freeze cycle/rate only sprint/swim/walk/run/bob/idle/fidget. Restore `playbackRate=1` on draw/holster/reload/fire/attack.
- Melee: `|vel|>1.1` new-swing edge; 10 Rodrigues +50° about controller right; hull ±16; range 56; origin = **viewmodel abs origin**. `dTraceRay` rewrite **removed**. CreateMove viewangles stay controller. `IN_ATTACK` 120 ms. `playbackRate=0` only on hit/miss/attack labels while melee.

Log tags to confirm: `CreateNamedRT … LITERAL` HMD-fit (not 2144 grow), no `Skip leftover … 2560x1440`, `FlashlightState -> HMD`, `Crowbar swing` without TraceRay rewrite.

**Do not claim HMD success.** User should confirm: desktop≈HMD, flashlight in HMD, FPS vs 47777b5, no warp.

## Pass 2026-08-28 weapon menu / MP5 / crowbar / haptics / hivehand / scale

**Compiled** (Win32 `d3d9.dll` 2699264 bytes + x64 OpenXR helper). **Installed** to DXVK folder, `bin\`, next to `bms.exe`, SteamVR manifest, and `VR\openxr_helper64`. **Not launched. Not HMD-verified.** Do not claim headset presentation.

Architecture unchanged: fused stereo, uncoupled VM, controller pose, `CUserCmd::weaponselect` (never `invnext`), existing freeze/`PulseAimHaptic`/DME scale.

| Item | Status | Notes |
|---|---|---|
| Weapon selection wheel | compiled + installed | Hold right stick click ≥180 ms. World-space HEV-amber icons in front of HMD. Right-controller ray hover glows. Release on icon → `m_PendingWeaponSelect` → CreateMove `weaponselect`. Release elsewhere cancels. Analog turn/jump/crouch/NextItem suppressed only while held. Short tap still recenters. SteamVR: `/actions/main/in/WeaponMenu` on right stick/trackpad click. OpenXR: `InventoryQuickSwitch` on the same right-stick click as `ResetPosition`. If SteamVR cached old bindings, restore BMVR defaults. |
| MP5 recoil | compiled + installed | Zero `m_vecPunchAngle` / vel and `m_recoilPunchAngles` / `m_recoilPositionOffset` / `m_recoilStartTime` on the owner while MP5 is active. Freeze MP5 fire-sequence cycle/rate (not reload/draw) so the remaining studio kick does not move the uncoupled VM. Other weapons unchanged. |
| Crowbar anims | compiled + installed | Crowbar/wrench only: freeze cycle/rate except draw/holster/deploy/pickup/admire. Idle/move/attack/swing stay pinned so the controller is the swing. Null sequence labels do not freeze (hdr may not be latched). |
| Fire haptics | compiled + installed | `UpdateWeaponFireHaptics`: pulse aim hand on `m_nMuzzleFlashParity` change or clip decrease. Not trigger-press, not a timer. Crowbar skipped here. |
| Crowbar-hit haptics | compiled + installed | Fan-trace hit still `PulseAimHaptic(3999)`. Swing-start pulse is not used. |
| Hivehand pose | compiled + installed | Root cause: identity `w_hgun` never matched `hive`/`hornet`, table was (0,0,0); `v_hgun` MainBody rest `(-11.03,-23.34,-15.69)` left the mesh ~0.6 m off the hand. Now match `hgun`/`hive`/`hornet`. Offset is rest compensation (`ox=restX`, `oy=-restY`, `oz=restZ`) from the AbsPos convention `p0 -= fwd*ox+right*oy+up*oz`, or live DME bake with X sign flipped. Not a random XYZ. Needs HMD check. |
| Grenade / revolver scale | compiled + installed | DME only: grenade/frag `×1.25`, 357/python/revolver `×1.15`. Global `g_ViewmodelScale` and crowbar `1.0` unchanged. |

Log tags: `Weapon menu opened`, `Weapon menu select entity=`, `Weapon menu cancelled`, `Viewmodel bake bone=MainBody`, `Viewmodel pose … ox,oy,oz=`.

**HMD checklist (not done):** hold right stick → readable wheel in headset; hover glow; release equips; analog turn still works when not held; MP5 no kick; crowbar visually still except draw; fire rumble on shot; crowbar rumble only on hit; hivehand on the hand; grenade larger, revolver slightly larger, crowbar unchanged.

## Pass 2026-08-28b hex menu / MP5 idle / crowbar idle / hivehand X

**HMD from previous install (user):** weapon **select works**; icons were a stacked row (hard to pick). **MP5 recoil still on.** Crowbar draw-only was good but **attack still posed** the mesh. **Haptics work.** Hivehand alignment better, still **too far forward**.

**This pass compiled + installed** (`d3d9.dll` 2712576). **Not HMD-verified.**

| Item | Change |
|---|---|
| Weapon menu | HL2VR-style **pointy-top hex honeycomb** with empty hub. Fixed category slots (pistols up, MP5/tau NE, shotgun/xbow east, crowbar down, RPG SW, gluon west, hive/nades NW). Native BM HUD icons from `bms_textures_dir.vpk` `materials/vgui/hud/weapon_*.vtf` (BGRA8888). Hover thickens the amber hex glow. Log: `Weapon menu HUD icons loaded=` |
| MP5 / crowbar | Root cause: freeze ran **after** `CalcViewModelView`, so SetupBones already used fire/attack **frame 0** (the kick/swing pose). Now force **idle sequence** (`idle1` / `IdleFidget`) **before** original CalcViewModelView. Crowbar equip/draw still play. MP5 reload/draw still play. Punch netvars still zeroed. Log: `Viewmodel force idle seq=` |
| Hivehand | Hull X `-3.6 .. +51.4`. `ox=restX=-11` put MainBody on the hand but the mesh still ~35 hu forward. Keep Y/Z rest (`oy=23.34`, `oz=-15.69`). **ox=24** (same in-hand pull as other VMs). |

Do not claim HMD success on 28b.

## Pass 2026-08-28c hex packing / HUD icons / hand-follow / MP5 bullets / numpad

**Compiled + installed this pass** (`d3d9.dll` 2727936). **Not HMD-verified.** Previous HMD: hexes packed on desktop, **gapped in the headset**; HUD icons were mostly orange squares (only pixel crowbar/glock); menu stayed world-locked while walking; MP5 **bullets** still climbed; numpad offsets were off.

| Item | Change |
|---|---|
| Hex packing | Draw radius is now `neighborPx / √3` from the same projection as the cell centers. The old 22–110 px clamp shrank HMD hexes while centers stayed FOV-projected. Thin gutter `0.97`. Same amber/dark fill, white hover outline (no hover scale). |
| Layout | 11 packed cells + **empty center = empty hands**. Inner ring of 6, outer 5 (12/10/2/8/4 o'clock, no 6 o'clock). Extra weapons overflow outside the flower. |
| Follow hand | Menu origin is recomputed every frame from the right controller + HMD basis (`hand + fwd*11 + up*5`). Walking moves it. |
| HUD icons | VTF decode now handles BGRA/RGBA/DXT1/DXT5. Upload is DXVK-safe `SYSTEMMEM` + `DEFAULT` + `UpdateTexture` (MANAGED lock was the likely miss). Alpha-crop so 256×128 HUD sprites fill the hex. Log: `Weapon menu HUD icons loaded=`. |
| Empty hands | Center hex on release sets `m_EmptyHands`: skip viewmodel DME, strip `IN_ATTACK`/`IN_ATTACK2`. Picking a weapon clears it. Not `ClientCmd`. |
| MP5 bullets | Punch/recoil netvars zeroed **and** controller `cmd->viewangles` applied **before** original CreateMove (FireBullets runs inside original). Post-hook alone was too late for predicted shots. |
| Numpad offsets | Per-weapon extras on top of the built-in table. Numpad 4/6 Y, 8/2 Z, 7/9 X; Ctrl = angles; 5 save to `VR/viewmodel_offsets.txt`; 0 reset this weapon; Shift = fine. Log: `Viewmodel numpad`. |

Do not claim HMD success on 28c.

## Pass 2026-08-28d baked poses / MP5 fire anim / menu lock / gloves / crowbar

**Compiled + installed this pass** (`d3d9.dll` 2728960). **Not HMD-verified.** Previous HMD: saved numpad poses; MP5 bullets still climbed; MP5 fire anim frozen; empty hands hid the right glove; weapon menu followed the hand; gloves sat behind the controllers; crowbar hair-trigger.

| Item | Change |
|---|---|
| Weapon poses | 2026-08-28 numpad extras baked into `ResolveWeaponViewmodelPose`. Grenade/satchel/tripmine/snark and gauss/gluon are separate rows. Install overwrites `VR/viewmodel_offsets.txt` so extras do not double. Numpad still adds on top. |
| MP5 fire anim | Removed idle-force on MP5 fire sequences. Crowbar idle-force is unchanged. **Never freeze gun fire anims.** Punch netvars still zeroed; also `sv_suppress_viewpunch 1` + `sv_viewpunch_spring_constant 0`. |
| Empty hands | Right HEV glove draws when `m_EmptyHands` (no grip-curl, no gun palm offset). Right mesh is always warmed up. |
| Weapon menu | Latches origin/basis at open (still spawned near the hand). After that it translates with player body and rotates only with stick yaw (`m_RotationOffsetY`). Hand can move freely to ray-select. |
| Glove forward | `VrHandsPoseOffsetMeters` Z `-0.10` (~60% of a ~16 cm scaled hand along aim; controller basis Z is -forward). Default + config + installer updated together. |
| Crowbar | Swing on `>2.4 m/s`, off `<0.9 m/s`, 400 ms cooldown. No longer resets NewSwing on every dip below 1.1. |

Do not claim HMD success on 28d.

## Pass 2026-08-28e OpenXR head-turn jitter (compiled + zipped; not HMD-verified)

**Compiled** (`d3d9.dll` 2729472) and packed `dist/Black-Mesa-VR-drop-in.zip`. **Not HMD-verified.**

Default path is `VRRuntimeBackend=openxr` + helper. SteamVR **Motion Smoothing** is an OpenVR compositor feature, so toggling it does nothing here. The helper was locating views at `predictedDisplayTime` and attaching those poses to the **previous** game eye image. That makes ATW/reprojection assume the late image matches the current head pose → jitter on turn.

Fix: snapshot the OpenXR HMD pose used for the stereo `RenderView`, publish it with the eye textures, and set `OpenXRHelperUseGameRenderPoseForProjection=true` so `xrEndFrame` uses that pose. Helper binary already supported the flag; d3d9 now publishes the pose. **Not HMD-verified.**

## Pass 2026-08-28f weapon wheel sounds / muzzle origin / fire haptics

**Compiled + installed** (`d3d9.dll` 2736128 bytes) to DXVK folder, `bin\`, and next to `bms.exe`. Install stopped a running `bms.exe`. **Not launched. Not HMD-verified.** Do not claim headset presentation.

| Item | Change |
|---|---|
| Weapon wheel sounds | Hover queues `Player.WeaponSelectionMoveSlot` (on hover-index change only). Confirm queues `Player.WeaponSelected` (including empty hands). Played from CreateMove via `CBaseEntity::EmitSound` (`client.dll+0x1D80D0`) on the local player. Not called from Present. |
| Controller fire haptics | VR `IN_ATTACK` / analog / weaponselect are applied **before** original player CreateMove (FireBullets runs inside it). Mouse already had buttons set; controller fire did not, so clip/parity never updated and rumble was skipped. After CreateMove, clip/parity haptic still runs, plus a trigger-edge pulse for energy weapons that do not decrement clip (hivehand/TAU). |
| Shoot origin | Hook client `Weapon_ShootPosition` (`+0x7C030`, thunk to EyePosition `+0x268`, **not** EyePosition itself) and listen-server `server.dll+0x11E490`. Rewrite to viewmodel `muzzle`/`Fire01`/`Fire02`, else controller-held VM origin. Hivehand hornets and TAU beam start used this origin. |
| Muzzle flash align | `SetAbsOrigin` on the viewmodel at CreateMove so attachments are not a frame behind. `GetAttachment` vec/matrix (`+0x97A80` / `+0x979B0`) scale local-VM attachments the same way DME scales the mesh (`ViewmodelScale`). |

Log tags: `Weapon_ShootPosition VR muzzle`, `Hook enabled: client Weapon_ShootPosition`, `Hook enabled: server Weapon_ShootPosition`, `Hook enabled: GetAttachment`.

**HMD checklist:** wheel hover/select uses the game HUD sounds; muzzle flash sits on the barrel; hivehand hornets leave the held model; TAU beam starts at the gun; shooting with the trigger rumbles the aim controller (mouse fire should still rumble).

Do not claim HMD success on 28f.

## Pass 2026-08-28g fire rumble / wheel draw sound / HUD scale / secondary ammo

**Compiled + installed this pass** (`d3d9.dll` 2739200 bytes). **Not launched. Not HMD-verified.** Do not claim headset presentation.

| Item | Change |
|---|---|
| Fire rumble | CreateMove only *sets* `m_PendingFireHaptic` (clip/parity + trigger edge). Pulse runs from ProcessInput (Present thread), which is the path that already rumbles reload/use. OpenXR duration maps OpenVR µs clicks to 60–160 ms (`xrApplyHapticFeedback`); 2.2 ms was below SteamVR's feelable floor. |
| Weapon wheel sound | Hover is 2D `common/wpn_moveselect.wav` via `IEngineSound::EmitAmbientSound`. Select plays `weapon_*.Draw` on the weapon entity (the gun's own deploy wav from `bms_misc`). Empty hands: `common/wpn_hudoff.wav`. No more `CBaseEntity::EmitSound(Player.WeaponSelected)` on the player (that is the spatialized HUD emit). |
| Honeycomb size | Menu origin still latches to body + stick yaw. Hex plane billboards to the HMD every frame so head tilt does not foreshorten. Pixel radius is `kHexRadiusHu / z / tanHalf` (constant world size). |
| Wrist HUD size | Digit size is the same world-HU projection (not a fixed fraction of the RT). Head tilt no longer changes apparent size vs the glove. |
| Wrist secondary ammo | `m_iSecondaryAmmoType` + `m_iAmmo[]`, else `m_iClip2` (Ghidra `DT_LocalWeaponData` 0xA60 / 0xA68). Drawn as `SEC` next to reserve (MP5 grenades). |

Do not claim HMD success on 28g.

## Pass 2026-08-28h honeycomb gaps / constant size / equip sound / fire rumble / wrist scale

**Compiled + packed this pass** (`d3d9.dll` 2738688 bytes, `dist/Black-Mesa-VR-drop-in.zip`). **Not HMD-verified.** Do not claim headset presentation.

| Item | Change |
|---|---|
| Honeycomb | Screen-space layout around the projected menu origin. Pixel radius is `34px` at 1440p (smaller). Packing `×1.16` leaves a gap. Edges are `0.22×` radius. Size does **not** use view-space z, so head tilt/lean no longer scales or overlaps the hexes. Origin still follows body + stick yaw. |
| Equip sound | Select (gun or empty hands) plays 2D `common/wpn_hudoff.wav` — the empty-hands click. Hover stays `wpn_moveselect`. |
| Fire rumble | Trigger-press pulse from ProcessInput (same path as reload) on **both** hands, amplitude 1. OpenXR now sends 160 Hz and ≥80 ms (helper no longer treats 0 Hz as a no-op). Clip/parity pending pulse kept for autos. Log: `OpenXR haptic` / helper `xrApplyHapticFeedback ok`. |
| Wrist HUD | Back to pixel scale at `0.72` of the original 1440p digits (world-HU path was huge). Secondary ammo kept. |

Do not claim HMD success on 28h.

## Pass 2026-08-28i wrist HUD size / right-hand offset / honeycomb ring / fire rumble

**Compiled + packed this pass** (`d3d9.dll` 2739200 bytes, `dist/Black-Mesa-VR-drop-in.zip`). **Not HMD-verified.** Do not claim headset presentation. Honeycomb look was HMD-confirmed good on 28h follow-up; haptics miss was empty batteries.

| Item | Change |
|---|---|
| Wrist HUD size | Pixel scale `1.44` at 1440p (2× the compact `0.72`). Segment thickness `4.8` / `3.4` (was `3.6` / `2.5`). Labels use 1.35× pixel fill. |
| Right ammo HUD | World offset is controller **left** (`-right × 4.5` HU), not down the forearm. Screen cluster is left-aligned so SEC's right edge sits at the grip. Left health HUD stays on the forearm. |
| Honeycomb | Continuous hex ring, 76px radius at 1440p, packing `×1.20` (shipped in the same zip as the wrist pass). |
| Fire rumble | Right-hand per-shot pulse from Present clip/parity. No left-hand trigger-press pulse. |

Do not claim HMD success on 28i.

## Pass 2026-09-03 stereo bloom skip / weapon wheel world-lock

**Bloom (HMD-verified ghosts, skip compiled + installed previously):** Anomalous Materials cafeteria (`bm_c1a0a`) fluorescent copies on the dark ceiling and a table/NPC stamp on the olive wall were **bloom** (`engine_post` / `lumcompare` / `downsample` / `blurfilter` / `blur_combine`). God rays, `bms_postprocess`, xog, DOF are not the cause. Current path skips that bloom DSSR on stereo eyes; the rest of post stays on.

Do **not** retry:

| Attempt | Result |
|---|---|
| Dest-expand `engine_post` to eye + write stored `Viewport` so `GetViewport` FLerp matches | Ghosts stayed |
| Grow `_rt_Small*FB*` to eye/N **and** force `DoEnginePostProcessing` x/y/w/h to eye | Ghosts gone; **picture zoomed and warped with head** |
| Revert only `DoEnginePostProcessing`; keep SmallFB grow | Same zoom/warp |

Do not grow `_rt_gb*2`. Do not Y-flip 2D capture. HEV charger trail D3D clear, StretchRect HWND→POT, shader-constant rewrite, CopyRTEx eye srcRect grow, viewport/scissor expand, `nr_gbuffer_for_refraction_enabled 0`, motion blur off, god rays on — keep. Reflection gbuffer is on again with full water.

**Weapon wheel this pass:** spawn at the right-hand grip (not 11 HU down the aim ray) and draw/hover in the latched world plane. Screen-pixel honeycomb around a projected origin was why the wheel tilted and slid with the HMD. Snap-turn still yaws with `m_RotationOffsetY`. **Compiled + installed** (`d3d9.dll` 2788352 bytes). **Not HMD-verified.**

Do not claim HMD success. User should confirm: opens at the hand; stays put when looking around (no tilt); hover/select still works.

## Pass 2026-09-03 water refraction / main menu VR

**Compiled + installed this pass** (`d3d9.dll` 2791424 bytes). **Not HMD-verified.** Do not claim headset presentation.

| Item | Change |
|---|---|
| Water | `r_WaterDrawRefraction 1` (QoL + `VR/bmvr.cfg`). Cheap-water force 0 was a flat teal fog fill. Planar **reflection** and `nr_gbuffer_for_refraction/reflection` stay **off** (view-locked ghost / wall stamps). Water/Refract named RTs still not grown to the eye. |
| Main menu in HMD | Skip-file `menu_vr` is ignored (it permanently hid `background*` capture after a 2026-08-16 hang). 2D capture Submit retries on any LevelInit map. Crash-sticky still disables **one** launch if Present dies. Look/CreateMove stay off on `background*`. |
| Menu controllers | On `background*` maps, right-controller aim maps onto the captured 16:9 menu; trigger / MenuSelect click; left-menu/Pause is Escape. Does not require the game window to be foreground (SteamVR usually has focus). |

Log tags: `Menu compositor begin`, `VR menu click`, `Ignoring menu_vr skip-file`, `waterrefract1`.

**HMD checklist:** main menu visible in the headset before loading a map; point and click New Game / Load; water in Xen/coast has a surface (not a solid fill); no view-locked world in the water; no gbuffer refraction stamps on walls.

Do not claim HMD success.

## Pass 2026-09-03b menu empty-map submit / full water

**Compiled + installed this pass** (`d3d9.dll` 2794496 bytes). **Not HMD-verified.**

User: menu still black until level load. Log (~11k Presents): `inGame=0 eligible=0 map=` `createdRT=0`. `-oldgameui` never names `background01`, so the map-name gate never opened. `menu_vr` skip was already ignored (`menuVR=1`).

| Item | Change |
|---|---|
| Menu Submit | `ShouldCompositorSubmit` no longer requires a map name on the GameUI. 2D capture + OpenXR publish from the first healthy Present. Skip `bmvrHUD` CreateNamedRT until a gameplay map. |
| Menu cursor | Same no-map GameUI path. |
| Water | Full: `r_WaterDrawReflection` / `r_waterforcereflectentities` / `r_waterforceexpensive` / `nr_gbuffer_for_reflection_enabled`. Keep `nr_gbuffer_for_refraction_enabled 0`. |

Log tags: `Menu compositor begin map=(none)`, `CreateVRTextures begin`, `PrePresent capture`, `OpenXR mono capture copied to both eyes`, `waterfull`.

**HMD checklist:** menu in the headset at GameUI (before New Game); point+click; water has reflections without wall stamps.

Do not claim HMD success.

## Pass 2026-09-03c menu cursor off Present-thread VGUI

**Compiled + installed this pass** (`d3d9.dll` 2794496 bytes). **Not HMD-verified.**

User: crash shortly after reaching the menu. Log: `Menu compositor begin map=(none)`, CreateVRTextures 3168×3104, OpenXR publishing, ~286 fps to present 704, last line `controller poses L=1 R=1`. Submit itself survived; the first controller-tracking `Update` called VGUI `IInput` (`SetCursorPos` / `InternalCursorMoved`) from the DXVK Present thread.

| Item | Change |
|---|---|
| Menu cursor | HWND `SetCursorPos` + throttled `WM_MOUSEMOVE` / click. No VGUI IInput from Present. |
| menu_vr sticky | Stays armed until a gameplay `LevelInit` (was cleared at 120 submits). |

Log tags: `Menu cursor HWND`.

**HMD checklist:** GameUI stays up in the headset; point+click New Game; no crash when controllers track.

Do not claim HMD success.

## Pass 2026-09-03d menu visible + stick nav

**Compiled + installed this pass** (`d3d9.dll` 2798592 bytes). **Not HMD-verified.**

User: menu still black in the HMD, but cursor/nav worked on desktop. Log: one `PrePresent capture 2560x1440`, then `Frame copy RT ready 3168x3104`, then OpenXR kept publishing that empty eye RT. OpenXR never cleared `m_FrameCopyLatched`, so GameUI was never recaptured.

| Item | Change |
|---|---|
| Menu image | Keep capture at HWND 2560×1440. Letterbox into the 3168 eyes. Clear the copy latch after each OpenXR publish. |
| Stick nav | Left stick = arrows. **A** = Enter. **B** / Pause = Escape. Trigger still point-clicks. |

Log tags: `OpenXR mono capture letterboxed 2560x1440`, more `PrePresent capture`, `Menu nav`, `Menu confirm A/MenuSelect`.

**HMD checklist:** old GameUI visible in the headset (letterboxed 16:9); stick moves the highlight; A confirms; B goes back.

Do not claim HMD success.

## Pass 2026-09-03e menu Submit stayed off

**Compiled + installed this pass** (`d3d9.dll` 2799616 bytes). **Not HMD-verified.**

User: still completely black. Last log: `Disabled menu/background compositor Submit this launch only`, `menuVR=0`, `createdRT=0`. Helper `submitted=0`. The letterbox/recapture DLL never ran — quitting GameUI left `bmvr_in_menu_vr.flag`.

| Item | Change |
|---|---|
| menu_vr sticky | Ignored. `BeginRisky` only covers `CreateVRTextures`. |
| GameUI Submit | Always on (empty map). Capture prefers the HWND backbuffer. |
| Menu FOV | Publish `renderFovXDeg=0` so the helper uses the HMD frustum. |

Log tags: `Ignoring menu_vr crash-sticky`, `Menu compositor begin`, `submit=1`, `createdRT=1`, `PrePresent capture bb`, `OpenXR mono capture letterboxed`, more than one capture.

**HMD checklist:** old GameUI visible in the headset before New Game.

Do not claim HMD success.

## Pass 2026-09-03f menu distance + cursor

**Compiled + installed this pass** (`d3d9.dll` 2802176 bytes). **Not HMD-verified.**

User: GameUI visible, but too close to read, no mouse cursor in VR, pointer too shaky.

| Item | Change |
|---|---|
| Distance | Letterbox at `MenuPanelScale=0.44` (was full-width / on your face). |
| Cursor | Yellow arrow drawn into the captured menu. Windows cursor is not in the backbuffer. |
| Smooth | 0.18s low-pass; flicks still track. Point at the smaller panel. |

Config: `MenuPanelScale` (lower = farther), `MenuCursorSmoothSec` (higher = steadier).

Do not claim HMD success.

## Pass 2026-09-03g pause 2D + cursor color

**Compiled + installed this pass** (`d3d9.dll` 2804736 bytes). **Not HMD-verified.**

User: panel too far; no pause menu in VR; Windows cursor only on desktop; arrow should match BM orange / Blue Shift blue.

| Item | Change |
|---|---|
| Distance | `MenuPanelScale=0.56` (was 0.44). |
| Cursor | HEV orange, or Calhoun blue if `-game bshift`. Drawn on the 2D panel. |
| Pause | Same HWND 2D capture as the main menu. Do not blit BB into the eyes then overwrite with a stale copy. |

Log tags: `Pause 2D panel capture`, `Pause 2D letterboxed`.

Do not claim HMD success.

## Pass 2026-09-03h fixed menu + save list

**Compiled + installed this pass** (`d3d9.dll` 2809856 bytes). **Not HMD-verified.**

User: still too far and follows the head; save list needs mouse-wheel; double trigger should load like double-click.

| Item | Change |
|---|---|
| Distance | `MenuPanelScale=0.70` |
| Fixed | Latch HMD pose when the 2D panel appears; look around it. Recenter relatches. |
| Saves | Right stick Y = `WM_MOUSEWHEEL`. Double trigger = `WM_LBUTTONDBLCLK`. |

Log tags: `Menu panel pose latched`, `Menu wheel`, `VR menu double-click`.

Do not claim HMD success.

## Pass 2026-09-03i level menu + stereo after save

**Compiled + installed this pass** (`d3d9.dll` 2811392 bytes). **Not HMD-verified.**

User: tilted head → tilted menu; after loading a save the desktop menu closes but the HMD stays 2D until pause is toggled.

| Item | Change |
|---|---|
| Upright | Latch yaw only (drop headset pitch/roll). |
| After load | 2D panel only while GameUI is actually visible (`VEngineVGui001`), not `IsPaused`. Clear our pause latch on LevelInit. |

Log tags: `Menu panel pose latched (level yaw`, `GameUI dismissed (engine hide)`.

Do not claim HMD success.

## Pass 2026-09-04 full water was the FPS regression (user-verified)

Same spot: GitHub **47 FPS**, local with full water **32 FPS**. Cheap-water revert (`waterrefl0`) restored GitHub fps. Hands and VR menu were not the cause.

Do **not** retry:

| Attempt | Result | Retry |
|---|---|---|
| `r_WaterDrawRefraction/Reflection 1` + `r_waterforcereflectentities 1` + `r_waterforceexpensive 1` + `nr_gbuffer_for_reflection_enabled 1` | Planar water inside each stereo `ViewDrawScene` (~10 ms) | **no** |

Do not retry that **full** set. `nr_gbuffer_for_refraction_enabled` stays 0. Do not grow Water/Refract RTs to the eye.

**2026-09-06 (user):** `r_WaterDrawRefraction 1` alone costs no noticeable FPS and looks better. QoL + `VR/bmvr.cfg` default is refraction-only again (`waterrefract1`). Reflection / expensive / gbuffer reflection stay 0.






## Pass 2026-09-06 performance (CPU / GPU sync)

**Compiled + installed this pass** (`d3d9.dll` 2898944 bytes, all three locations; `OpenXrDeferredPublish` / `OpenXrSlotCoolingMs` added to the installed `VR\config.txt`). **Not HMD-verified.** Full list with rationale: `docs/performance-pass.md`; OpenXR publish redesign: `docs/openxr-eye-publish.md` §7.

| Item | Change |
|---|---|
| OpenXR publish | No `WaitDeviceIdle` per publish. Eye copies fenced with `D3DQUERYTYPE_EVENT` + new fork `IDirect3DVR9::FlushCommands()`, published on a later Present when done. `OpenXrDeferredPublish=true` / `OpenXrSlotCoolingMs=4` in `VR/config.txt`. `false` restores the old path. |
| OpenVR IPC | `TickCompositorFocus` 10 Hz, `PollSteamVrRecommendedSize` 4 Hz, FullFrame `FindTexture` probe 20 Hz. |
| D3D per-frame | RT0 / backbuffer pointers tracked from hooks instead of `GetRenderTarget`/`GetBackBuffer`; capture-on-unbind gated; shader-constant hook copy-on-write; hand pass uses one recorded state block instead of `D3DSBT_ALL` per hand per eye; HUD glyphs batched. |
| Hooks CPU | Viewmodel class flags (crowbar/mp5/rpg/gluon/shotgun/scoped) replace per-call `strstr`; RT-name class memo; sequence-label + idle-seq caches; `FindRpgLaserDot` rescan <= 4 Hz; sprite hooks early-out; `dTraceRay` passthrough removed; `Ensure*Hooks` done flags; cached `client.dll` handle. |
| Hands | `HandRig` per GLB asset; curl palette + palm light cached per hand per Present. |
| Win32 / I/O | HWND + client rect cached; `BeginRisky`/`EndRisky` touch the flag file only on state change; one `OutputDebugStringA` per log line. |

Log tags: `OpenXR publish rate ... deferred= throttled= dropped= pending= maxPending=`.

**HMD checklist:** gameplay looks the same as before; no ghosting/tearing on head turn (if so, set `OpenXrDeferredPublish=false` and compare); gloves animate and match lighting in both eyes; crowbar swing, RPG laser dot, gluon beam, MP5 offset unchanged; Present interval variance lower than the previous build.

**HMD run 1 (user, 2026-09-06, WMR G2 90 Hz, RTX 4070 Ti):** higher fps than `false`, but tearing on head turns and judder while walking in low-fps areas; reprojection no longer hid drops. Log: `present=74-84/s published=67-72/s maxPending=2 dropped=1-3/s`. Diagnosis + fix (`docs/openxr-eye-publish.md` §7a), **compiled + installed, not HMD-verified**:

| Item | Change |
|---|---|
| Run-ahead | `OpenXrMaxPending=1` (new key, 1-3). Present blocks on the previous copy's event (`WaitOldestOpenXrPending`, 250 ms timeout -> `WaitDeviceIdle`) before copying new eyes. Pose age <= 1 GPU frame, `dropped` should be 0. Rate log: `paceWait=/s avg= max=`. |
| Slot reuse | Bridge **v14**: helper writes `helperConsumingFrameId` before and `helperConsumedFrameId` after its two `RenderEye` blits (queue idle). Game never rewrites a slot whose published frame id > consumed. Timer stays as margin; gate dropped if helper silent > 500 ms. Slots 3 -> 4. Rate log: `helperHold= helperFeedback= consumedLag=`. |
| Helper queue | `VK_KHR/EXT_global_priority` HIGH on the helper's graphics queue, fallback to default on `NOT_PERMITTED`. Helper log: `Vulkan graphics queue global priority: ...`. |

**HMD run 2 (user, still super stuttery, 2026-09-06):** `consumedLag=4-8`, HIGH rejected, compositor ~60 Hz in-world vs ~90 Hz in menu. Helper `vkQueueWaitIdle` twice before `xrEndFrame` missed vsync, so WMR could not reproject. Game `WaitOldestOpenXrPending` also stalled Present. **Compiled + installed, not HMD-verified** (`docs/openxr-eye-publish.md` §7b): helper blits without waiting (3 CBs + fence; EndFrame on time; consumed on fence); game skips a Present instead of blocking on the copy event.

**HMD run 3 (user, even worse):** `present=61/s published=30/s helperHold=30/s`. One-in-flight backpressure + deferred publish skipped every other Present. Reverted 11 ms cap, consume hold, and 4 ms EndFrame wait.

**HMD run 4 (user, still stuttery vs pre-pass):** `OpenXrDeferredPublish=true` never matched the old headset feel. Default is `false` again (game `WaitDeviceIdle` + helper `vkQueueWaitIdle` before `xrEndFrame`). CPU caches from this pass stay. See `docs/openxr-eye-publish.md` §7d.

**HMD run 5 (user, 2026-09-06):** `OpenXrDeferredPublish=false` **definitely feels better** than `true`, even though `true` reports higher FPS. WMR motion smoothing / reprojection needs a regular finished pair on vsync; extra game frames that miss compositor cadence show up as stutter. Keep `false`.

## Pass 2026-09-20 VR frame pacing (compiled; not HMD-verified)

**Symptom A:** desktop ~180–320 FPS and smooth, HMD not native 90 Hz (reprojection-like motion).
**Symptom B:** occasional single broken/old frame.

**Diagnosed cause (logs 2026-09-20, WMR G2 90 Hz, `bm_c2a2a`):** this is not a graphics-settings problem and not "probably reprojection" without evidence.

| Evidence | Meaning |
|---|---|
| Game `present=186-228/s published=186-228/s skipRate=0 helperFeedback=0 consumedLag=-1` | Engine firehoses pairs; consumed-id gate never armed on the default sync path |
| Helper `Submitted OpenXR Vulkan frame` ~60/s in-world (60 frames / ~1s) | Compositor client missing 90 Hz vsync; runtime must ATW/reproject the other 30 Hz |
| Helper `sourceGen` skips (22→26) | Extra published frames are dropped, not displayed |
| `OpenXrDeferredPublish=false` round-robin 4 slots at ~200/s (~20 ms wrap) vs two `vkQueueWaitIdle`s per eye | Slot can be overwritten while the helper is still blitting — occasional torn/old frame |

**HL2VR difference:** `OnFrameBegin` → `WaitGetPoses` on the present timeline → render with that pose → submit-thread `Submit_TextureWithPose` → `PostPresentHandoff`. One frame in flight. We had an uncapped OpenXR publish loop in another process.

**This pass (handshake, compiled + installed):** OpenXR waits meant to match that lifecycle (`docs/openxr-eye-publish.md` §9). Game waited for this WaitFrame pose and previous slot consume before stereo. One pair per `displayTime`. Helper 9 ms match poll + one queue idle, then `xrEndFrame`.

**HMD run 1 (user, 2026-09-20, WMR G2 90 Hz):** performance worse; felt reprojection still present; glitched frames much less often. Logs: helper `matchHit=2–6/N` `matchWaitAvg=9.01ms` (timeout every vsync), `waitFrameAvg=12.5–15.5ms`; game `handoffWait` every published frame, `await avg=5–15ms max=16–47ms`, `published≈helperHz≈44–78`, `helperFeedback=1`, `consumedLag=1–3`. Menu `published=0/s skipVsync=258` (latched panel pose). Slot gate is the glitch win. The waits are the 2026-08-30 “do not Sleep after xrWaitFrame” miss.

**Follow-up 1 (compiled + installed; HMD run 2):** removed helper 9 ms poll and game 16/32 ms consume waits. Slot gate kept. User: FPS improved, still heavy reprojection, not true 90 Hz. In-game: `present=150-250/s` `await=130-190/s` 3k stereo, `helperHz` 89→45, `helperHold` climbing, helper lifetime `waitFrameAvg` 14.9 ms and blitSkip/fenceBusy climbing. Extra stereo saturated the GPU so the compositor blit missed vsync.

**Follow-up 2 (HMD-verified 2026-09-20, WMR G2 90 Hz):** one stereo pair per helper `xrWaitFrame` pose (≤22 ms wait, no consume wait); skip extra 3k pairs; helper ping-pong blit CBs/fences. User: native 90 Hz. Logs: `present=180/s published=90/s await=90/s avg=3.3–3.8ms helperHz=90 helperFeedback=1 helperHold=0`; helper `waitFrameWin=11.11ms waitFrameLast=11.09ms blitSkip=0 fenceBusyFrames=0`.

**Tab-out crash (same session):** pause then alt-tab → helper `exitCode=1`, no `bmvr_crash.txt`. Game log: `GameUI visible` → VGui extra-paint overlay → `MC Present fallback: ICallQueue did not copy 1x eyes`. No `App deactivated (WM_ACTIVATEAPP)` — same class as 2026-09-13 (`docs/RUNTIME.md`).

**Follow-up 3 (compiled this pass; pause+tab not verified):** skip Present eye StretchRect whenever in-game pause overlay is up (`WantPauseWorldOverlay`, not all GameUI — main-menu 2D panel still copies); HUD overlay only while HWND activated; skip all copies on GameUI + deactivate; `WM_ACTIVATE` earlier than `ACTIVATEAPP`; no 22 ms pose-wait during pause overlay; extra-paint original + HUD TransferSurface SEH. Do not skip copies on focus-loss without GameUI (SteamVR).

Do not claim full gameplay success.

## Pass 2026-09-21 review (compiled this pass; not HMD-verified)

Fresh pass over the live tree. Stereo fusion, OpenXR cadence (`OpenXrDeferredPublish=false`, one pair per helper `xrWaitFrame`, slot consume gate), flashlight RT policy, bloom skip, and water refraction-only were left as already verified. Do not reopen those.

| Item | Change |
|---|---|
| Cross-process seqlock | Eye-pair publish already fenced. Pose, input, game-render pose, frame ids, and the HUD overlay did not. MSVC can move those payload stores outside the odd generation window, which is a torn head pose or a torn overlay handle. Writers now use `L4D2VR_SeqlockBegin` / `End`. Readers acquire before and after the copy. Overlay submit uses `ReadOverlay` instead of a bare struct load. |
| Slot reuse flag | Helper wrote `helperConsumedFrameId` and then `helperConsumedCount`, but the game acquired **after** loading both. A new count could pair with the previous frame id and free a slot the blit still held. Count is the flag: write the id, release, then increment count. The game loads count, acquires, then loads the id. |
| RT stack | A push past 32 entries returned without a slot, and the matching pop still removed a real entry, so aux/world class tracked the wrong target for the rest of the frame. Overflow pushes now have their own pop count. Log: `RT stack overflow`. |
| Pose velocity clock | `ConsumeOpenXrTracking` called `QueryPerformanceFrequency` on every sample, including the compositor-pose wait. It uses the cached `QpcNowMs` clock. Menu cursor smoothing uses that same clock. |

**Not changed:** frame pacing waits, fused eye size, bloom skip, water reflection, flashlight RT resize, `mat_queue` off.

**Build:** Release Win32 `build\Release\d3d9.dll` and x64 `build_openxr_helper64\Release\OpenXRHelper64.exe` compiled clean (pre-existing C4099/C4319 only). **Not installed. Not launched. Not HMD-verified.** Ship both binaries together; the seqlock change is in the shared bridge.

**Still open:** HMD flashlight (do not retry the rejected RT / 16:9 paths). Pause then alt-tab (`WantPauseWorldOverlay` copy skip) still needs a headset confirm. These sync fixes need a headset confirm: no torn frame on a fast head turn, pause menu still appears.
