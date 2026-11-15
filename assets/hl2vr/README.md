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

The extracted HL2VR `w_*` world models, left-handed duplicates, weapon scripts
and `sound.cache` files are deliberately not shipped. Copying HL2VR's
`scripts/game_sounds_weapons.txt` into `bms/scripts/` would replace Black Mesa's
weapon sound script, so keep it out unless you are deliberately testing that.

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
