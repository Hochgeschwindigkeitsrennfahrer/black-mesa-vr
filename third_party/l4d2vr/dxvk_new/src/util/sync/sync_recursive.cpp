#include "sync_recursive.h"
#include "sync_spinlock.h"

namespace dxvk::sync {

  void RecursiveSpinlock::lock() {
    spin(2000, [this] { return try_lock(); });
  }


  void RecursiveSpinlock::unlock() {
    if (likely(m_counter == 0))
      m_owner.store(0, std::memory_order_release);
    else
      m_counter -= 1;
  }


  bool RecursiveSpinlock::try_lock() {
    uint32_t threadId = dxvk::this_thread::get_id();

    // BMVR: re-entry by the owner (nested d3d9 calls) without a locked
    // read-modify-write, and no cmpxchg while another thread holds it. Only
    // the owner thread ever stores its own id or touches m_counter.
    const uint32_t owner = m_owner.load(std::memory_order_relaxed);
    if (owner == threadId) {
      m_counter += 1;
      return true;
    }
    if (owner != 0)
      return false;

    uint32_t expected = 0;

    bool status = m_owner.compare_exchange_weak(
      expected, threadId, std::memory_order_acquire);
    
    if (status)
      return true;
    
    if (expected != threadId)
      return false;
    
    m_counter += 1;
    return true;
  }

}
