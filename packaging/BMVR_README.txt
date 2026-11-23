Black Mesa VR
=============

A free, unofficial VR mod for the Steam version of Black Mesa (and Blue Shift).
Full guide, screenshots and bug tracker:
  https://github.com/Hochgeschwindigkeitsrennfahrer/black-mesa-vr


1) What you need
----------------
- Black Mesa on Steam, up to date on the default branch (no beta).
  On Linux, use the Proton version of the game, not a native build.
- A PC VR headset with an OpenXR runtime: SteamVR, Meta Link,
  Virtual Desktop (VDXR), WMR and others.


2) Install
----------
1. Quit Black Mesa. A running game does not pick up the new files.
2. Copy EVERYTHING inside this folder into your Black Mesa folder, the one
   that contains bms.exe (Steam -> Black Mesa -> Manage -> Browse local
   files). Replace files when asked.
   This puts d3d9.dll in all three places it has to be:
     d3d9.dll
     bin\d3d9.dll
     bin\thirdparty\dxvk-windows-x86\d3d9.dll
3. Steam -> Black Mesa -> Properties -> Launch Options:
     -heapsize 524288 -processheap -high -novid -windowed -oldgameui
   -oldgameui is required (the new game UI is upside-down in the headset).
   If the game's video menu shows "Direct3D 9" as the renderer, also add
   -enabledxvk.
4. Turn off Steam Input for Black Mesa
   (Properties -> Controller -> Disable Steam Input).
5. Pick ONE OpenXR runtime and start it before the game:
   - SteamVR: SteamVR -> Settings -> OpenXR -> Set SteamVR as OpenXR runtime.
   - Meta Link (Quest): uses the Link / Oculus OpenXR runtime.
   - Virtual Desktop: Streamer -> Options -> OpenXR Runtime = VDXR.
     "SteamVR" in that dropdown shows the world upside-down.
6. Launch Black Mesa from Steam. bmvr_log.txt next to bms.exe should appear
   and start with "BMVR d3d9.dll loaded".

Blue Shift: same install, plus -game bshift in the launch options.

Updating: copy the new files over the old install the same way. Keep your
VR\config.txt unless the release notes say otherwise; it only holds what
you changed.


3) Default controls
-------------------
PS VR2 button names first, Quest / Touch names in brackets.

Left controller                     Right controller
  Stick         move                  Stick         turn; up jump,
  Stick click   sprint (toggle)                     down crouch (hold)
  Trigger       use                   Stick click   weapon menu
  Grip          hold a gun with       Trigger       fire
                two hands             Grip          flashlight
  Triangle (Y)  pause;                Circle (B)    alt-fire
                hold: VR settings     Cross (A)     reload
  Square (X)    previous weapon

Grab gestures: reach behind a shoulder, to a hip or (left hand) to your
forehead and squeeze grip. By default the right hand draws the crowbar
(right shoulder), pistol (right hip) and MP5 (left hip); the left hand
quick saves (left shoulder), takes ammo (left hip) and toggles the
flashlight (forehead).

Menus: point the right controller at them and pull the trigger.

Every button and gesture can be changed in the VR settings panel.


4) VR settings panel
--------------------
Hold Triangle (Y) for about half a second, or pick the gear in the weapon
menu. Tabs: General, Hands & Weapons, Gestures, Controls, Graphics, Debug.
Most changes apply right away and are saved into VR\config.txt.

Graphics tips:
- Foveated rendering: Eye-tracked (default). It falls back to the lens
  centre on headsets without eye tracking.
- MSAA 2x if your GPU has room. Keep FXAA on next to it (Sharp preset).


5) Settings only in VR\config.txt
---------------------------------
A few settings set up the eye images when the game starts, so they are
only in this file. Quit the game fully before you edit it.

  RenderScale=1.0     Multiplier on the headset's recommended eye size.
                      1.2-1.4 = supersampling: sharper, much less shimmer,
                      at a GPU cost. 0.8-0.9 if you are GPU-bound.
  MaxEyeSize=4096     Largest eye side in pixels. Raise only if a high
                      RenderScale hits it (bmvr_log.txt shows the size used).
  IPDScale, HeightOffset    world scale and player height.

The file explains every setting in comments.


6) If it does not work
----------------------
- No bmvr_log.txt next to bms.exe: the mod did not load. Check that the
  game was closed while you copied, that d3d9.dll is in all three places,
  and add -enabledxvk if the video menu shows Direct3D 9.
- Steam "Verify integrity of game files" puts the game's own d3d9.dll
  back in the DXVK folder: copy the mod files again afterwards.
- World upside-down with Virtual Desktop: set its OpenXR Runtime to VDXR.
- The first launch after an update recompiles shaders: expect some hitches
  in the first minutes.

Reporting a bug: attach bmvr_log.txt (next to bms.exe). For visual problems,
VR settings -> Debug -> Capture debug frame while the problem is visible
helps a lot. Please report problems to our issue tracker on GitHub, not to
Crowbar Collective or Valve.


7) Uninstall
------------
Quit the game, then delete in the Black Mesa folder: d3d9.dll,
BMVR_README.txt, bmvr_* files, the VR folder, bin\d3d9.dll,
bms\cfg\bmvr.cfg and bms\sound\bmvr (same under bshift if you played
Blue Shift). Installs from before 2026-11 may also have openvr_api.dll next
to bms.exe, bin\thirdparty\dxvk-windows-x86\openvr_api.dll and
bin\sourcevr.dll.disabled: delete those too.
Then Steam -> Black Mesa -> Properties -> Installed Files -> Verify
integrity of game files, and remove the launch options.


Disclaimer
----------
This is an unofficial, free fan project and is not affiliated with,
endorsed by, or supported by Crowbar Collective or Valve Corporation. You
need a legitimate copy of Black Mesa. This mod does not include or
distribute any Black Mesa or Valve game files, code or assets, and it is
not sold. Black Mesa is a trademark of Crowbar Collective. Half-Life is a
trademark of Valve Corporation.
