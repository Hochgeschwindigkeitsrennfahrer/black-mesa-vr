#pragma once

#include <atomic>

#include "../com/com_include.h"

namespace dxvk::sync {

  /**
   * \brief Recursive spinlock
   * 
   * Implements a spinlock that can be acquired
   * by the same thread multiple times.
   */
  class RecursiveSpinlock {

  public:

    void lock();

    void unlock();

    bool try_lock();

    // BMVR lock profiling: current owner thread id (0 = free).
    uint32_t owner() const { return m_owner.load(std::memory_order_relaxed); }

  private:

    std::atomic<uint32_t> m_owner   = { 0u };
    uint32_t              m_counter = { 0u };
    
  };

}
