#pragma once

class Game;

namespace bmvr
{
    // Player-facing VR settings as console variables (bmvr_*). Defaults come
    // from VR/config.txt; a cvar only writes its flag when it changes, so the
    // in-VR settings panel (which saves to VR/config.txt) is not overridden.
    //
    // Call from the game thread (RenderView). First call registers the cvars
    // (once client.dll's cvars exist), later calls copy their values into the
    // bmvr::g_* flags the VR code reads.
    void TickVrCvars(Game* game);
}
