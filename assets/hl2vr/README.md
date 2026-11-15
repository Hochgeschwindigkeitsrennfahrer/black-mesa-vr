# HL2VR weapon and hand assets

`content/` is an install-ready `bms/` tree for the HL2VR viewmodels and hands
used by the `hlvr-pistol` branch. `scripts/install.ps1` copies it into
`Black Mesa\bms\`, and `scripts/pack_release.ps1` includes it in the drop-in
zip. No manual extraction is required.

Source models, textures and sounds come from **Half-Life 2 VR** by wormslayer
(`Half-Life 2 VR\hlvr`), with the pistol/slide/magazine pack calibrated for
Black Mesa. Half-Life 2 VR is a separate project; these files are shipped here
so a fresh clone can render the same weapons and hands.

## What is included

| Path | Used for |
|---|---|
| `models/weapons/vr_pistol.*` | HL2VR pistol (glock replacement) |
| `models/weapons/vr_pistol_slide.*` | Pistol slide |
| `models/weapons/vr_pistol_clip.*` | Pistol magazine |
| `models/weapons/vr_hlvr_hands.*` | HL2VR hands (renamed `player.mdl`), grip/support hands |
| `models/weapons/vr_crowbar.*` | Physical crowbar |
| `models/weapons/vr_shotgun.*` + `vr_shotgun_foregrip.*` | Shotgun and pump forend |
| `models/weapons/vr_smg.*` + clip/foregrip/charginghandle | MP5 and moving parts |
| `models/weapons/vr_grenade.*` | Hand grenade |
| `materials/` | Viewmodel/hand materials, detail and normal maps |
| `sound/weapons/` | Pistol slide/reload, shotgun, MP5 and crowbar wavs |

The `.vmt` files are the tuned ones used by the last Black Mesa test run
(bump/normal maps, phong, rim light, fabric and metal detail), not the flat
one-line stubs that ship in the original asset packs. The crowbar material was
completed from `Half-Life 2 VR\hlvr` because the pack only carried the `.vmt`
and not the `vr_crowbar_d/nrm/exp` maps it points at.

## What is deliberately not shipped

| Omitted | Why |
|---|---|
| `w_*.mdl` world models and `materials/models/world_models/w_*` | The mod only swaps the stock viewmodels (`v_crowbar/v_shotgun/v_mp5/v_grenade.mdl`, see `WeaponForNeedle` in `src/vr_hlvr_weapons.cpp`). Shipping the HL2VR world models would replace Black Mesa's world/NPC weapon art (HECU shotgun, dropped pistol) with HL2 models the mod never draws. |
| `models/weapons/left_handed/*_l.*` | `bmvr::g_LeftHanded` only remaps controller roles and aim (`src/vr.cpp`); no `_l` model path is ever loaded. |
| `scripts/weapon_*.txt`, `scripts/game_sounds_weapons.txt` | These are HL2/HL2VR tables. Copying them into `bms/scripts/` replaces Black Mesa's weapon data (viewmodel, clip size, bucket) and its whole weapon sound table. The code loads model paths directly and plays wavs by path, so it needs neither. |
| `sound/sound.cache` | Generated index of a sound folder. A cache made on another install does not list these wavs, which makes them silently unplayable. The engine rebuilds it. |
| `materials/models/weapons/vr_pistol/*` | HL2VR's original pistol materials. The calibrated model's material name is `v_glock`, so these are unused. |
| `materials/models/player/player*.vtf` (67 MB) | HL2VR's own player-model set. The hands model shipped here uses `v_hand`/`v_hand_m`. |
| `models/items/grenadeammo.*` | Ammo-pickup entity model; only the held `vr_grenade.mdl` is drawn, Black Mesa keeps its own pickup. |
| `content.vpk`, `hlmvplusplus.dxvk-cache`, `unpack-log.txt`, `*.jpg`, `* - copy.vtf` | Packing artifacts, previews and duplicate textures. |

## Installed locations

`scripts/install.ps1` writes into:

```text
Black Mesa\bms\models\weapons\vr_*
Black Mesa\bms\materials\models\player\
Black Mesa\bms\materials\models\weapons\v_glock\
Black Mesa\bms\materials\models\weapons\vr_crowbar\
Black Mesa\bms\materials\models\weapons\vr_grenade\
Black Mesa\bms\materials\models\weapons\vr_shotgun\
Black Mesa\bms\materials\models\weapons\vr_smg\
Black Mesa\bms\materials\detail\
Black Mesa\bms\sound\weapons\
```

The models are loaded by name from
`src/vr_hlvr_pistol.cpp` and `src/vr_hlvr_weapons.cpp`; no VPK extraction is
needed at runtime.
