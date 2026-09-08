# Black Mesa VR

<a href="https://www.youtube.com/watch?v=S9kmK95yqgM">
  <img src="assets/release-2026-09-06.jpg" alt="Black Mesa VR — Weapon Menu" width="90%">
</a>

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/P5P56X4HH)

A VR mod for the Steam version of **Black Mesa**, bringing 6DOF motion controls, true stereo rendering, OpenXR, and VR-first weapon interaction to the game.

The foundation of the VR implementation is based heavily on the approach pioneered by **L4D2VR**, adapted and extended for Black Mesa.

L4D2VR source:
https://github.com/keyou91/l4d2vr

## Current Status

**The project is working start-to-finish and is playable as a full VR mod.**

* True stereo rendering
* 6DOF motion controls
* HL2VR-style weapon selection
* Accurate crowbar swinging (HL2VR)
* OpenXR implementation
* Health and armor on the hand 
* Weapon-mounted ammo counter
* Per-hand item holding and use
* In-world transparent pause menu
* Bare hands until HEV (and throughout Blue Shift)
* Crossbow zoom (headset aim while scoped)
* VR main menu with a motion-controller cursor
* Blue Shift HUD theme (weapon wheel and wrist HUD)
* Multicore rendering

## Known Issues

The VR experience is fully playable, but several features are still a work in progress:

* **Quest 3:** Meta Link uses the Link / Oculus OpenXR runtime. Virtual Desktop must use Streamer Options → **OpenXR Runtime = VDXR**. SteamVR in that dropdown inverts the world
* If shots misalign with the weapon, press the right thumbstick once to recenter. Roomscale support is coming.
* Two-handed weapons are planned for a later update.
* Manual reloading is not currently implemented and is planned for a future update.

## Black Mesa: Blue Shift

Black Mesa VR also supports **Black Mesa: Blue Shift**.

After installing Blue Shift, launch the game with:

```text
-game bshift
```

The same VR setup can then be used with the Blue Shift campaign. Calhoun never
wears the HEV suit, so this build keeps the bare-hand models for the whole
Blue Shift session. The weapon wheel and wrist HUD use Calhoun blue instead of
HEV amber.


## Huge thanks wormslayer for HL2VR & to my testers for their patience and help:

TheRealBubble

SilentVortiguant - [Check out his youtube](https://www.youtube.com/@SilentVortigauntVR).

yakupagagaming
