#pragma once

// Short clips cut from Black Mesa's own weapon sounds (VR reload steps).
namespace bmvr::soundcuts
{
    enum Clip
    {
        kGlockMagOut,   // glock reload.wav: magazine out
        kGlockMagIn,    // glock reload.wav: new magazine pushed in
        kGlockSlide,    // glock reload.wav: slide
        kMp5MagOut,
        kMp5MagIn,
        kCrossbowMagOut,
        kCrossbowMagIn,
        kMp5Bolt,       // MP5 reload_long.wav: charging handle pulled back
        kClipCount
    };
    // Sample name for PlayUiSound ("bmvr/..."), or fallback when the clip
    // could not be made. Makes the clips on the first call (game thread).
    const char* Sample(Clip clip, const char* fallback);
}
