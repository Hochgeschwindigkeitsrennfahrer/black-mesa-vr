# Developing Black Mesa VR

How to build the mod, run your build in the game, test it without a headset, and port it to a new Black Mesa
update. Rules for changing the code are in `AGENTS.md` at the top of the repo. AI coding agents read that file
first; `CLAUDE.md` loads it for Claude Code.

The day-to-day setup is Linux with the game running under Proton. Everything below except the headless test
harness also works on Windows, but only the Linux build is tested regularly.

## 1. Build

**You need:** CMake 3.20 or newer, Ninja, LLVM/Clang (`clang-cl`, `lld-link`, `llvm-lib`, `llvm-rc`, `llvm-mt`),
and the MSVC CRT + Windows SDK headers and libraries, which [xwin](https://github.com/Jake-Shadle/xwin) downloads:

```sh
cargo install xwin --locked
xwin --accept-license --arch x86,x86_64 splat --output ~/xwin   # gives ~/xwin/crt and ~/xwin/sdk
export XWIN=~/xwin
```

**Configure once, then build:**

```sh
cmake -S . -B build-beta -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/clang-cl-xwin-x86.cmake
cmake -S OpenXRHelper64 -B build-beta-helper -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/clang-cl-xwin-x64.cmake

ninja -C build-beta d3d9      # build-beta/d3d9.dll (32-bit: the mod + DXVK)
ninja -C build-beta-helper    # build-beta-helper/OpenXRHelper64.exe (64-bit OpenXR helper)
```

A full DLL build compiles all of DXVK and takes a few minutes; after that ninja rebuilds only what changed. The build
fails on a bare `__try` (use `BMVR_TRY`, see `AGENTS.md`). If you move or rename a build directory, configure it
again with `cmake --fresh`: CMake keeps absolute paths.

**Windows (untested lately):** Visual Studio 2022 with CMake, `cmake -S . -B build -A Win32` and
`cmake --build build --config Release`; the helper with `-A x64`.

## 2. Run your build in the game

1. Install a release zip once (README → Install), so the game has the `VR\` folder, the helper, and the configs.
2. **Close the game.** Copy `build-beta/d3d9.dll` to all three places: next to `bms.exe`, `bin\`, and
   `bin\thirdparty\dxvk-windows-x86\`. Never copy over a DLL a running game has loaded: write a new file, then
   rename it over the old one.
3. If you changed `OpenXRHelper64/` or `src/openxr_bridge_protocol.h`, also copy `OpenXRHelper64.exe` to
   `VR\openxr_helper64\`. The DLL and the helper must come from the same commit when the protocol changes.

**Where to look:**
- `bmvr_log.txt` next to `bms.exe`: the mod's log. The header names the commit; `Offset checks <dll>: N/N ok` shows
  the game build matched.
- `VR\config.txt`: player settings, read at startup. The in-headset settings panel writes only the values you change.
- Console cvars `bmvr_*` (type `find bmvr_` in the console): most switches, also for A/B tests.
- VR settings panel → Debug → **Capture debug frame**: saves the game and dumps both eyes plus the render state, so a
  bug can be reproduced at the exact spot (see 3).

## 3. Test without a headset (Linux, Proton)

`tools/headless/` runs the real game under Proton with a fake VR runtime (`VRRuntimeBackend=fake`,
`src/vr_fake_helper.cpp`): scripted head and controller poses and buttons, both eye images dumped as BMP. Nothing is
written to your game folder: the run uses an overlay, and the scripts check afterwards that the folder and the Steam
Cloud config are unchanged.

**You need:** Steam running with Black Mesa installed (Proton Experimental), SteamVR **not** running, Xvfb,
unprivileged user namespaces (`unshare`), ImageMagick, Python 3. Make a copy of the game's Proton prefix once:

```sh
H=~/bmvr-harness; mkdir -p $H
cp -a ~/.local/share/Steam/steamapps/compatdata/362890 $H/compatdata
```

**A run:**

```sh
cat > $H/look.txt <<'EOF'
cmd god
fake hmd 0 0 1.70 0 0 0
wait 5
dump look 1
wait 2
log
EOF
BMVR_SETTINGS=live BMVR_XVFB_GPU=1 BMVR_PREFIX=$H/compatdata BMVR_DLL=$PWD/build-beta/d3d9.dll \
  timeout -k 20 200 tools/headless/bm-scenario $H/look.txt $H/out-1 -condebug +sv_cheats 1 +map bm_c1a0a
ls $H/out-1/VR/look-*.bmp; less $H/out-1/bmvr_log.txt
```

The scenario starts once the mod has drawn its first stereo pair in a map, and the game is closed when the last
line is done, so end with a `wait` after a `dump`. Use a fresh output directory per run.

| Scenario line | Effect |
|---|---|
| `cmd X` | console command |
| `fake K ...` / `unfake K` | set or remove one line of the fake runtime (below) |
| `wait S` | sleep S seconds |
| `dump name [step]` | both eyes + HUD as `VR/<name>-left/right/hud.bmp`; step 1 = full size, default 4 = quarter |
| `shot name` | screenshot of the game window |
| `log` | copy `bmvr_log.txt` now |
| `waitlog <regex> [s]` | wait for a log line (fails the run on timeout) |
| `trace N` / `audit N` | log every render pass / render-target use for N frames |

Fake runtime keys (metres, x forward, y left, z up; pitch yaw roll in degrees): `hmd`, `left`, `right`, `size w h`,
`fov out in up down`, `ipd`, `move x y`, `turn x y`, `buttons ...` (attack attack2 reload jump duck use recenter pause
flashlight select back next prev weaponmenu sprint gripleft; held until the next `buttons` line).

**Environment:** `BMVR_DLL` (your build; default the installed one), `BMVR_CONFIG_SET="Key=Value ..."` (config
overrides), `BMVR_EYE_SIZE="w h"` (default PS VR2's 2884×2940), `BMVR_FAKE_FOV`, `BMVR_W`/`BMVR_H` (window,
default 1920×1080), `BMVR_KEEP_UPPER=1` (keep what the game wrote), `BMVR_PUT="src:game/relative/dst"` (extra files;
no spaces in paths). `BMVR_SETTINGS=live` uses your current game settings; the other profiles (`vr`, `flat`) come from a
settings-switching launcher that is not part of this repo. `tools/headless/bm-restore <capture>-state.txt <outdir>`
puts a run at the exact spot and view of a panel capture.

**Know the limits:** `setang` does nothing in VR (aim with `fake hmd`); saves from an older game build silently stay in
the menu; comfort, aim feel and reprojection can only be judged in a headset. Say which settings a test used.

## 4. After a Black Mesa update

Game updates move code and data, so the addresses in `src/offsets.h` must be ported. Before Steam installs the update,
copy the old build's DLLs (or fetch them later with DepotDownloader, Windows depot 362894) into a folder with the game
layout: `bin/engine.dll`, `bin/materialsystem.dll`, `bin/shaderapidx9.dll`, `bms/bin/client.dll`,
`bms/bin/server.dll`. Then (needs `pip install capstone`):

```sh
python3 -I tools/offsets/port_offsets.py --old <old dll folder>           # dry run: read PROBLEMS and layout gates
python3 -I tools/offsets/port_offsets.py --old <old dll folder> --write
python3 -I tools/offsets/gen_offset_checks.py                             # regenerate src/offsets_checks.inc
ninja -C build-beta d3d9
```

Boot the game (or a headless run with `+map`) and check `Offset checks <dll>: N/N ok` for every module. Anything listed
as ambiguous or not found must be fixed before release. Every new address belongs in `src/offsets.h`; run
`gen_offset_checks.py` after adding one.

## 5. Other tools

| Tool | Use |
|---|---|
| `tools/vpk.py` | list (`<x_dir.vpk> <regex>`) or extract (`<x_dir.vpk> <a,b,c> <outdir>`) files from the game's VPKs |
| `tools/vpk_entities.py` | print a map's entity lump from `bms_maps_dir.vpk` |
| `tools/mdl_bones.py`, `dump_v_mdl.py`, `dump_mdl_attachments.py` | inspect models (bones, meshes, attachments) |
| `tools/dump_pcf.py` | inspect particle files |
| `tools/check_rva.py` | check the bytes at an address in a game DLL before hooking it |
| `tools/bake_clip_hand_poses.py` | regenerate `src/vr_clip_hand_poses.h` from the game's reload animations |
| `tools/headless/panel_aim.py` | controller angles to click a settings-panel row in a headless run |

Run Python tools with `python3 -I`, and keep extracted game files in their own folder.
