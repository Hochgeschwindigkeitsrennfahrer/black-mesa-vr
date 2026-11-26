# Black Mesa VR — agent notes

VR mod for the Steam version of Black Mesa (and Blue Shift). One 32-bit `d3d9.dll` (DXVK fork + mod code) plus a
64-bit OpenXR helper process. Player docs: `README.md`. **Build, run, headless tests, porting to a game update:
`docs/DEVELOPING.md`.** Approaches that failed and must not be revived blindly: `docs/RETIRED.md`.

## Layout

| Path | What |
|---|---|
| `src/` | All mod code: `dllmain`, `game` (interfaces, offsets), `hooks` (engine/client hooks), `vr*` (stereo, input, hands, weapon wheel, settings panel), `sdk/` (Source SDK types). `offsets.h` holds every RVA/signature for the current game build |
| `OpenXRHelper64/` | x64 helper: owns the OpenXR session, receives the eye textures through shared handles (`src/openxr_bridge_protocol.h`, shared with the DLL — change both together) |
| `third_party/l4d2vr/dxvk_new` | DXVK fork with `IDirect3DVR9` (from [L4D2VR](https://github.com/keyou91/l4d2vr)). CMake builds only the files listed in `tools/dxvk_sources.txt`. Upgrade playbook: `docs/DXVK-UPGRADE.md` |
| `L4D2VR/` | Include shims only (DXVK `#include "L4D2VR/game.h"`). No new code here |
| `VR/` | Files shipped into the game's `VR\` folder: `config.example.txt` (documented settings, never read: the player's `config.txt` is written by the mod and holds only their changes), `bmvr.cfg`, weapon-wheel textures, glove GLBs (fallback only: hands are built from the game's own models at runtime, `src/vr_bm_hands.cpp`) |
| `packaging/` | `BMVR_README.txt` for the release zip |
| `tools/offsets/` | Port `src/offsets.h` to a new game build (`port_offsets.py`, then `gen_offset_checks.py`) |
| `tools/headless/` | Headless test runner under Proton (`bm-scenario`, `bm-run`, `bm-restore`; fake VR runtime in `src/vr_fake_helper.cpp`) |
| `tools/*.py` | Game-file helpers: VPK list/extract, map entities, model/particle dumps, RVA checks |

## Build and test

```sh
export XWIN=~/xwin   # xwin splat of the MSVC CRT + Windows SDK (docs/DEVELOPING.md §1)
ninja -C build-beta d3d9 && ninja -C build-beta-helper
```

Before calling a change done: it builds, a headless run (`docs/DEVELOPING.md` §3) shows the expected result in both
eyes and a clean `bmvr_log.txt`, and anything about feel or comfort is handed to someone with a headset.

## Rules

1. **Install the DLL in all three places:** next to `bms.exe`, `bin\`, and `bin\thirdparty\dxvk-windows-x86\` (the
   launcher's DLL search skips the DXVK folder when the video menu is on native D3D9). Close the game first; never copy
   over a DLL a running game has loaded.
2. **Guarded memory reads use `BMVR_TRY`** (`src/bmvr_seh.h`), never a bare `__try`: on 32-bit clang-cl a caught fault
   otherwise wipes the caller's EBX/ESI/EDI. The build fails on a bare `__try`.
3. **Offsets change with every game update.** Port them with `tools/offsets/`; the log line
   `Offset checks <dll>: N/N ok` confirms them at boot.
4. **Keep player config minimal.** The zip ships no `VR/config.txt` and no dev tuning files (`viewmodel_offsets.txt`,
   `ammo_hud.txt`): defaults live in code, `VR/config.example.txt` documents the file-only settings (commented out),
   and the player's `config.txt` holds only what they changed. New tunables get a code default (and a settings-panel
   row if players need them). Upgrades never overwrite a player's `config.txt`. Do not set
   video-menu ARCHIVE cvars from code or `bmvr.cfg`: they save into `config.cfg` and reset the video preset.
5. **The desktop game must still start** when VR init fails.
6. **Status words mean what they say:** compiled / runs headless / verified in the headset. Headless runs cannot judge
   comfort, aim feel or reprojection; say so instead of claiming "fixed".
7. Black Mesa uses a deferred renderer: world render targets (FullFrame, G-buffer) must match the size of the view
   drawn into them. Read `docs/RETIRED.md` before changing eye sizes or the stereo path.
8. **Render bugs: localise, then instrument every pass in one build** (`trace` / `audit` in a headless run, or the
   panel's Capture debug frame) before guessing or A/B toggling. Racy bugs: repeat over several game starts and report
   the hit rate.
9. **A fix for one scene should cost nothing elsewhere:** prefer one-time state (e.g. a material flag at load) over a
    hook that runs on every draw.
10. **Rarely used fallback render paths** (old video settings, odd hardware) get a log warning and a request for a bug
    report, not full support.
