#pragma once

// Controller pictures for the VR panel's CONTROLS tab and the button labels
// shown on the controllers in your hands.
//
// The models are SteamVR render models (OBJ + PNG + JSON per controller),
// read from the user's own SteamVR / driver folders at runtime. Nothing is
// shipped with the mod. We read the files ourselves instead of using
// IVRRenderModels, because the OpenXR helper path never calls VR_Init.
//
// A worker thread loads both hands' models and renders each once, on the CPU,
// into a small picture (side view from the thumb side, ring down, like the
// PS5 button assignment screen). The panel pastes the picture into its GDI
// canvas, so nothing is drawn per frame. Each button part also gets a point in
// OpenXR grip space, which puts labels on the real controllers.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bmvr::ctlmodel
{
    enum Part : int { kTrigger = 0, kGrip, kFace1, kFace2, kStick, kMenu, kPartCount };

    struct Picture
    {
        int w = 0, h = 0;
        std::vector<uint32_t> px;             // 0xAARRGGBB, straight alpha (0 = background)
        float anchorX[kPartCount] = {};       // part centre in picture pixels
        float anchorY[kPartCount] = {};
    };

    struct HandModel
    {
        bool ok = false;
        bool gripKnown = false;               // the model has an openxr_grip part
        std::string name;                     // folder name, e.g. playstation_vr2_sense_left
        Picture picture;
        bool has[kPartCount] = {};
        float gripPoint[kPartCount][3] = {};  // part centre, OpenXR grip space (metres)
        float devicePoint[kPartCount][3] = {};// part centre, model (OpenVR device) space
        float gripCenter[3] = {};             // middle of the model, grip space
        float deviceCenter[3] = {};
    };

    struct Models
    {
        std::string key;
        HandModel hand[2];                    // 0 = left, 1 = right
    };

    // Starts a background load when the controller (or picture size) changed;
    // cheap otherwise. Call it whenever the panel is open.
    void Request(uint32_t family, const char* systemName, int picW, int picH);
    // Loaded models (either hand may still be !ok), or null while loading or
    // when nothing was found.
    std::shared_ptr<const Models> Current();
    // Changes whenever Current() changes.
    int Generation();
}
