# Black Mesa VR

<a href="https://www.youtube.com/watch?v=S9kmK95yqgM">
  <img src="assets/release-2026-09-06.jpg" alt="Black Mesa VR — Weapon Menu" width="90%">
</a>

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/P5P56X4HH)

A VR mod for the Steam version of **Black Mesa**: true stereo rendering, 6DOF motion controls, OpenXR, and
weapon handling built for VR. The whole campaign is playable from start to finish, and **Black Mesa: Blue Shift**
works too.

The VR foundation is based on the approach pioneered by **[L4D2VR](https://github.com/keyou91/l4d2vr)**, adapted and
extended for Black Mesa, with weapon selection, crowbar swinging and more adapted from **HL2VR**.

A macOS version lives here: https://github.com/armax9/black-mesa-vr-macos

**Download:** [Releases page](https://github.com/Hochgeschwindigkeitsrennfahrer/black-mesa-vr/releases). Take the
newest release that matches your Black Mesa build (the release notes say which build).

## Features

### Playing
- **6DOF motion controls**, the gun follows your hand, and you aim with the controller while you walk where you look.
- **Two-handed weapons:** put your off-hand on the forend of a rifle, shotgun, MP5, crossbow and so on and hold grip;
  the glove sits on the gun and steadies it.
- **Quick-draw gestures:** right hand at the right hip + grip = pistol, right hand above the left hip + grip = MP5,
  right hand behind the shoulder + grip = crowbar, left hand behind the shoulder (hold until a light buzz) + grip = quick save.
- **Crowbar swinging** that hits where you swing (HL2VR style).
- **HL2VR-style weapon wheel**, with the VR settings gear below the crowbar.
- **Grenades thrown at the speed of your hand.**
- **Crossbow scope built for VR:** bring the crossbow to your face with both hands to zoom; the zoomed view follows the
  crossbow, with smoothing for hand tremor.
- **Mounted guns** (50 cal, TOW) you steer with your hands.
- **Ladders you can grab**, per-hand item holding and use.
- **Health, suit and ammo on your hands**, an ammo counter on the weapon, and a **red flash at the edges of the view
  when you get hit**.
- **Aim marker and laser:** red laser dot, laser beam with adjustable width, or the classic amber ticks; all optional.
- **Flashlight** on the right grip, optionally in the left hand or on the left Square/X button.
- Snap or smooth turning, crouch while held or toggle, sprint toggle, recenter, cutscene cameras that can move your
  view (or not, if you prefer comfort).
- **Bare hands until you get the HEV suit** (and throughout Blue Shift), HEV gloves after that. A "classic flat hands"
  option brings back the original look.
- **VR main menu and pause menu** in the world, with a motion-controller pointer, and Black Mesa's loading screens
  as a flat panel.

### Image quality and performance
- **True stereo rendering** with each eye's own lens shape (great on asymmetric headsets such as the PS VR2), at the
  headset's full resolution or above it.
- **Foveated rendering:** the edges of each eye are shaded at a lower rate, which you hardly notice in the headset
  but gives a large speed-up in heavy scenes. **Eye-tracked** on headsets with eye tracking (the sharp area follows
  where you look), fixed at the lens centre on headsets without it. On by default.
- **MSAA on the eyes** (2x / 4x): much smoother edges on walls, pipes, cables, models and the gun, optionally also on
  leaves and fences.
- **FXAA tuned for VR**, with Sharp / Balanced / Smooth presets.
- **Less shimmer:** calmer cubemap reflections on shiny walls and floors (Full / Balanced / Subtle), lens flares,
  light glows and smoke that stay level and agree between the eyes.
- **MulticoreMode:** the game's render thread runs next to the game, and heavy outdoor areas keep a much higher
  frame rate than before.
- Many fixes for things that only break in VR: water and mirror reflections per eye, objects popping in one eye,
  glows shining through walls, Xen fog, and more.

### The VR settings panel
**Hold Triangle/Y** (about half a second) or pick the gear in the weapon wheel. Four tabs:
- **Controls:** turning, movement, grab gestures, flashlight, view, menus.
- **Hands & Weapons:** hand HUD and its position, hand pose, aim marker and laser, crossbow scope, grenades, mounted guns.
- **Graphics:** foveated rendering, frame cap, MSAA, FXAA, bloom, lens flares, light glows, reflections.
- **Debug:** diagnostics, logs and debug captures for bug reports.

Most changes apply right away. The panel only saves the settings you changed into `VR\config.txt`, so improved
defaults reach you with each update.

## Install

### What you need
- **Black Mesa on Steam**, updated to the build named in the release notes. On **Linux** use the **Proton** version
  of the game (Properties → Compatibility), not a native build.
- A PC VR headset with an **OpenXR runtime**: SteamVR, Meta Link, Virtual Desktop (VDXR), WMR and others.

### Steps
1. **Quit Black Mesa.** A running game does not pick up the new files.
2. Download the zip from the [Releases page](https://github.com/Hochgeschwindigkeitsrennfahrer/black-mesa-vr/releases)
   and extract it.
3. Copy **everything inside** `drop-into-Black-Mesa-folder` into your Black Mesa folder, the one that contains
   `bms.exe` (Steam → Black Mesa → Manage → Browse local files). Replace files when asked.
   This puts `d3d9.dll` in all three places it has to be: the game folder, `bin\` and `bin\thirdparty\dxvk-windows-x86\`.
4. Run **`DISABLE-SOURCEVR.bat`** once (next to `bms.exe`). It stops a "VR_Shutdown could not be located" popup at
   startup; this mod does not use Valve's old VR plugin.
5. **Steam → Black Mesa → Properties → Launch Options:**
   ```text
   -heapsize 524288 -processheap -high -novid -windowed -oldgameui
   ```
   `-oldgameui` is required (the new game UI is upside-down in the headset). If the game's video menu shows
   **Direct3D 9** as the renderer, also add `-enabledxvk`.
6. **Turn off Steam Input for Black Mesa** (Properties → Controller → Disable Steam Input).
7. Pick **one OpenXR runtime** and start it before the game:
   - **SteamVR:** SteamVR → Settings → OpenXR → Set SteamVR as OpenXR runtime.
   - **Meta Link (Quest):** uses the Link / Oculus OpenXR runtime.
   - **Virtual Desktop:** Streamer → Options → **OpenXR Runtime = VDXR**. "SteamVR" in that dropdown shows the
     world upside-down.
8. Launch Black Mesa from Steam. `bmvr_log.txt` next to `bms.exe` should appear and start with `BMVR d3d9.dll loaded`.

**Updating:** copy the new zip over the old install the same way. Keep your `VR\config.txt` unless the release notes
say otherwise; it only holds what you changed.

**Uninstall:** run `UNINSTALL-BMVR.bat` next to `bms.exe` and type `YES`. Steam's "Verify integrity of game files"
also restores the stock files.

**Blue Shift / Azure Sheep:** same install, plus `-game bshift` (or `-game asheep`) in the launch options. Calhoun
keeps bare hands for the whole campaign, and the weapon wheel and wrist HUD turn security-guard blue.

## Recommended setup

### In the VR settings panel (Graphics tab)
- **Foveated rendering: Eye-tracked** (default). The "Foveation now" row shows what it is actually doing: eye-tracked,
  fixed, or off because your GPU has no variable-rate shading. Without eye tracking it falls back to the lens centre
  by itself. Turning it on from off needs a game restart; the row tells you.
- **MSAA: 2x** if your GPU has room. It costs about 2 ms per frame in a heavy room on an RX 9070; 4x roughly
  doubles that. Keep **"MSAA on leaves + fences"** and **"MSAA skips particles"** on.
- **Keep FXAA on next to MSAA.** MSAA smooths geometry edges; it cannot reach Black Mesa's lighting, so thin lit
  objects (railings) and specular sparkle need FXAA. Start with the **Sharp** preset.
- **Reflections (shimmer): Balanced** (default). **Subtle** if shiny floors still shimmer, **Full** for the game's
  original look.

### In Black Mesa's video options
- **Anti-aliasing: FXAA** (the mod tunes it for the headset).
- **Water detail: Reflect all** if your PC can afford it. With "Reflect world", doors, signs and other objects are
  missing from water reflections, which shows as holes in some places (On A Rail).
- Lower shadow and light quality first if you run short on frame rate; those cost more in VR than in flat play.

### In `VR\config.txt` (not in the panel)
A few settings can only be changed in this file, because they set up the eye images when the game starts.
**Quit the game fully before you edit it**; it is read once at launch.

| Setting | Default | What it does |
|---|---|---|
| `RenderScale` | `1.0` | Multiplier on the headset's recommended eye size. **Above 1.0 = supersampling: sharper and much less shimmer**, at a GPU cost (1.2 renders about 44 % more pixels). If your PC can handle it, **1.2–1.4 is the biggest single image upgrade**; foveated rendering makes this a lot more affordable. Below 1.0 (0.8–0.9) if you are GPU-bound. |
| `OpenXRHelperDownsample` | empty | `box`: with RenderScale above 1.0, the mod scales the big eye image down to the headset size itself, for cleaner edges. Try it together with a higher RenderScale. |
| `MaxEyeSize` | `4096` | Largest eye side in pixels. Raise it (up to 5456 with MulticoreMode) only if a high RenderScale hits the cap; `bmvr_log.txt` shows the eye size that was used. Far above 4096 can run the 32-bit game out of memory. |
| `ReflectionRtScale` | `0.5` | Resolution of **water and mirror-floor reflections**, as a fraction of the eye size. Higher = sharper reflections and less shimmer at floor and wall edges, but costs GPU in rooms with big reflective floors. `0` = the game's fixed 1024×1024, which shimmers badly at headset resolution. |
| `RefractCopyRtScale` | `0.5` | Resolution of the screen copy that **shiny floors and wired glass** sample. At the game's 1024×1024 the edges where floors meet walls look jagged. It is only a copy, so it is cheap: leave it at 0.5. |
| `OffCenterEyes` | `true` | Renders each eye's own lens shape. Makes asymmetric headsets (PS VR2) much sharper at the same cost. Set to `false` only if the view looks wrong on your headset. |
| `IPDScale`, `HeightOffset` | `1.0`, `0.0` | World scale and player height tweaks. |

The file explains every setting in comments.

## Known issues
- **Thin lit objects stay jagged even with MSAA 4x** (railings, thin bars). Black Mesa lights the scene in a separate
  pass at 1x that MSAA cannot reach. FXAA softens it.
- **Odd reflections in door glass, windows and shiny surfaces** come from the maps' cubemaps, not the mod. The
  REFLECTIONS presets make them less visible.
- **Very heavy outdoor views can still drop below the headset's refresh rate:** the game draws every eye separately,
  and its render thread is the limit there.
- **Your hands disappear below the water surface** when you reach into water.
- **Stand-up and cutscene scenes rotate your view** with the scene's camera. Turn off
  **Controls → VIEW → Cutscene cameras move the view** if that bothers you.
- If shots don't line up with the gun, **recenter**: tap the right stick, or **Controls → VIEW → Recenter view**.
- **Manual (physical) reloading is not implemented**; reload is a button (A).
- The first launch after an update recompiles shaders: expect some hitches in the first minutes.

**Reporting a bug:** attach `bmvr_log.txt` (next to `bms.exe`). For visual problems, **Debug → Capture debug frame**
while the problem is visible helps a lot.

## Huge thanks to wormslayer for HL2VR, and to my testers and contributors for their patience and help:

marnovdm for quality of life features and polish, beta support, MSAA and foveated rendering (https://github.com/marnovdm)
TheRealBubble
SilentVortiguant - [Check out his youtube](https://www.youtube.com/@SilentVortigauntVR).
yakupagagaming
Discord user martysl1, whose eye-MSAA experiment made the MSAA feature possible.

Credits for the custom models:
https://steamcommunity.com/sharedfiles/filedetails/?id=2969302231 (HEV Gloves)
