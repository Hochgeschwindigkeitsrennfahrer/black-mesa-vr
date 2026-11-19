#pragma once

#include "vr_hand_types.h"

#include <string>

// Black Mesa's own first-person hands as glove meshes, built at startup from
// the installed game's VPKs (models/weapons/v_glock.mdl, "arms" body part).
// Nothing is shipped: every player converts their own copy. The result uses
// the same joint names and joint-frame convention as the HEV glove GLBs, so
// the finger-curl code treats it like them.
namespace BmHands
{
    enum class Variant
    {
        Hev,   // HEV suit gloves (v_hand)
        Bare,  // scientist bare hands (bare_hands_d), before the suit
    };

    // hand: 0 left, 1 right. scale: 1 = Black Mesa's viewmodel size.
    bool Build(Variant variant, int hand, float scale, VrHandMeshAsset& out, std::string& error);
    // HEV cuff cap for the gun's own hand (viewmodel arms): positions in the
    // R_/L_Wrist bone frame, model units, one joint. After a Hev Build.
    bool GunCuffCap(int hand, VrHandMeshAsset& out);
}
