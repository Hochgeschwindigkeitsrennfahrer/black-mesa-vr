#pragma once

#include <cstdint>
#include <string>

#include "../util/log/log.h"
#include "../util/util_env.h"

namespace dxvk {

  /**
   * \brief BMVR test aid: fixed fragment shading rate for every draw
   *
   * Foveated rendering Phase 1 (upper bound): shade whole render targets
   * at a coarse rate, no rate image yet. Off unless the environment
   * variable BMVR_VRS is set before the game starts:
   *   2x2 / 4x4        every graphics draw
   *   2x2eye / 4x4eye  only draws into eye-sized targets (>= 2400x2400)
   * With it unset nothing changes (feature not enabled, no extra state).
   */
  struct BmvrVrsMode {
    uint32_t rate    = 0;      // 0 = off, else 2 or 4 (square)
    bool     eyeOnly = false;
  };

  /**
   * \brief BMVR foveated rendering wanted (config Foveated != 0 at start)
   *
   * Set by the d3d9 layer before the device is created; enables
   * attachmentFragmentShadingRate (+ pipeline rate) on the device. The
   * per-eye rate image itself is bound with
   * DxvkContext::bmvrSetShadingRateImage.
   */
  inline bool& bmvrFoveationRequestedRef() {
    static bool requested = false;
    return requested;
  }

  inline bool bmvrFoveationRequested() {
    return bmvrFoveationRequestedRef();
  }

  // Test switch (env BMVR_FOV_SECONDARY=1 keeps secondary command buffers
  // for rate-image passes, the stock DXVK choice).
  inline bool bmvrFoveationPrimary() {
    static const bool primary = env::getEnvVar("BMVR_FOV_SECONDARY") != "1";
    return primary;
  }

  inline const BmvrVrsMode& bmvrVrsMode() {
    static const BmvrVrsMode mode = [] {
      BmvrVrsMode m;
      std::string v = env::getEnvVar("BMVR_VRS");
      if (v.rfind("2x2", 0) == 0) m.rate = 2;
      else if (v.rfind("4x4", 0) == 0) m.rate = 4;
      m.eyeOnly = m.rate && v.find("eye") != std::string::npos;
      if (m.rate)
        Logger::warn(str::format("BMVR VRS: fixed ", m.rate, "x", m.rate, m.eyeOnly ? " on eye-sized targets" : " on all draws"));
      return m;
    }();
    return mode;
  }

}
