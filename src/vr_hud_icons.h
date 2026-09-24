#pragma once

#include <d3d9.h>

// Shared access to the Black Mesa HUD icon atlas. Implemented in
// vr_weapon_menu.cpp, which already owns the VPK reader, VTF decoder and
// texture upload path used by the weapon wheel.
namespace bmvr
{
    // Loads materials/vgui/hud/<vtfName> from the game VPKs and uploads it as a
    // D3D9 texture. Cached by name; returns nullptr until the background
    // preload has uploaded that icon (or the asset is missing).
    IDirect3DTexture9* AcquireHudIcon(IDirect3DDevice9* device, const char* vtfName);

    // Decode/upscale weapon, ammo, and wrist icons off the eye thread.
    // Pump uploads one finished icon per call; call it once per stereo pair.
    void KickHudIconPreload();
    void PumpHudIconUploads(IDirect3DDevice9* device);

    // Ammo icons for the weapon the player is holding, keyed off its model /
    // network name. Never return null.
    const char* PrimaryAmmoIconVtf(const char* model, const char* net);
    const char* SecondaryAmmoIconVtf(const char* model, const char* net);
}
