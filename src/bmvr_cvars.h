#pragma once

class Game;

namespace bmvr
{
    // Player-facing VR settings as real console variables (bmvr_*), so they
    // can be changed from Options -> Game (bms/custom/bmvr/resource/
    // OptionsSubBlackMesa.res) or the console, and persist in config.cfg.
    // Defaults come from VR/config.txt; a value saved in config.cfg wins.
    //
    // Call from the game thread (RenderView). First call registers the cvars
    // (once client.dll's cvars exist), later calls copy their values into the
    // bmvr::g_* flags the VR code reads.
    void TickVrCvars(Game* game);
}
