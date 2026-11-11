#pragma once

#include "d3d9_include.h"

#include <atomic>
#include <thread>
#include "../../../../../src/bmvr_prof.h"

namespace dxvk {

  // BMVR PerfLog: time a contended device-lock acquisition (kind: 0 plain,
  // 1 exclusive, 2 Source frame ownership) and the drain of in-flight calls
  // (kind + 3).
  inline void BmvrTimedLock(sync::RecursiveSpinlock& mutex, int kind) {
    if (!::bmvr::g_PerfLog) {
      mutex.lock();
      return;
    }
    if (mutex.try_lock())
      return;
    const uint32_t owner = mutex.owner();
    const long long t0 = ::bmvr::prof::Now();
    mutex.lock();
    ::bmvr::prof::LockWait(kind, ::bmvr::prof::Now() - t0, owner);
  }

  template<typename Pred>
  inline void BmvrTimedDrain(Pred busy, int kind) {
    if (!busy())
      return;
    const long long t0 = ::bmvr::g_PerfLog ? ::bmvr::prof::Now() : 0;
    while (busy())
      std::this_thread::yield();
    if (::bmvr::g_PerfLog)
      ::bmvr::prof::LockWait(kind + 3, ::bmvr::prof::Now() - t0, 0);
  }

  /**
   * \brief Device lock
   *
   * Lightweight RAII wrapper that implements
   * a subset of the functionality provided by
   * \c std::unique_lock, with the goal of being
   * cheaper to construct and destroy.
   */
  class D3D9DeviceLock {

  public:

    D3D9DeviceLock() = default;

    D3D9DeviceLock(sync::RecursiveSpinlock& mutex)
      : m_mutex(&mutex) {
      BmvrTimedLock(mutex, 0);
    }

    static D3D9DeviceLock Activity(std::atomic<uint32_t>& activity) {
      D3D9DeviceLock result;
      result.m_activity = &activity;
      return result;
    }

    static D3D9DeviceLock Exclusive(
            sync::RecursiveSpinlock& mutex,
            std::atomic<uint32_t>& activity,
            std::atomic<uint32_t>& exclusiveRequests) {
      D3D9DeviceLock result;
      result.m_mutex = &mutex;
      result.m_exclusiveRequests = &exclusiveRequests;

      // Publish the writer request before taking the recursive lock. New ordinary
      // D3D9 calls then join the lock, while calls already executing remain visible
      // through the activity count below.
      exclusiveRequests.fetch_add(1, std::memory_order_acq_rel);
      BmvrTimedLock(mutex, 1);
      BmvrTimedDrain([&] { return activity.load(std::memory_order_acquire) != 0; }, 1);
      if (::bmvr::g_PerfLog)
        result.m_holdStart = ::bmvr::prof::Now();

      return result;
    }

    D3D9DeviceLock(D3D9DeviceLock&& other)
      : m_mutex(other.m_mutex),
        m_activity(other.m_activity),
        m_exclusiveRequests(other.m_exclusiveRequests),
        m_holdStart(other.m_holdStart) {
      other.Clear();
    }

    D3D9DeviceLock& operator = (D3D9DeviceLock&& other) {
      Release();
      m_mutex = other.m_mutex;
      m_activity = other.m_activity;
      m_exclusiveRequests = other.m_exclusiveRequests;
      m_holdStart = other.m_holdStart;
      other.Clear();
      return *this;
    }

    ~D3D9DeviceLock() {
      Release();
    }

  private:

    void Clear() {
      m_holdStart = 0;
      m_mutex = nullptr;
      m_activity = nullptr;
      m_exclusiveRequests = nullptr;
    }

    void Release() {
      if (m_holdStart != 0 && ::bmvr::g_PerfLog)
        ::bmvr::prof::LockHold(1, ::bmvr::prof::Now() - m_holdStart);
      if (m_activity != nullptr)
        m_activity->fetch_sub(1, std::memory_order_release);
      if (m_mutex != nullptr)
        m_mutex->unlock();
      if (m_exclusiveRequests != nullptr)
        m_exclusiveRequests->fetch_sub(1, std::memory_order_release);
      Clear();
    }

    sync::RecursiveSpinlock* m_mutex = nullptr;
    std::atomic<uint32_t>* m_activity = nullptr;
    std::atomic<uint32_t>* m_exclusiveRequests = nullptr;
    long long m_holdStart = 0;

  };


  /**
   * \brief D3D9 context lock
   */
  class D3D9Multithread {

  public:

    D3D9Multithread(
      BOOL                  Protected);

    D3D9DeviceLock AcquireLock() {
      // Source's material worker owns the device for the complete queued render
      // command interval. Calls made by that owner are already serialized by the
      // outer frame lock, so avoid reader registration on every draw/state call.
      if (SourceFrameOwner() == this)
        return D3D9DeviceLock();

      if (m_protected)
        return D3D9DeviceLock(m_mutex);

      for (;;) {
        if (m_sourceFrameActive.load(std::memory_order_acquire)
         || m_exclusiveRequests.load(std::memory_order_acquire) != 0)
          return AcquireExclusiveLock();

        m_unprotectedActivity.fetch_add(1, std::memory_order_acq_rel);
        if (!m_sourceFrameActive.load(std::memory_order_acquire)
         && m_exclusiveRequests.load(std::memory_order_acquire) == 0)
          return D3D9DeviceLock::Activity(m_unprotectedActivity);

        // A writer arrived between the first check and activity publication. Back
        // out and join the exclusive side; the writer waits for this decrement.
        m_unprotectedActivity.fetch_sub(1, std::memory_order_release);
      }
    }

    D3D9DeviceLock AcquireExclusiveLock() {
      if (m_protected)
        return D3D9DeviceLock(m_mutex);

      return D3D9DeviceLock::Exclusive(
        m_mutex,
        m_unprotectedActivity,
        m_exclusiveRequests);
    }

    bool BeginSourceFrameOwnership() {
      D3D9Multithread*& owner = SourceFrameOwner();
      uint32_t& depth = SourceFrameDepth();

      if (owner == this) {
        depth += 1;
        return true;
      }

      // A thread cannot execute two devices' Source frame command streams at the
      // same time. Treat that as a marker mismatch instead of unlocking the wrong
      // recursive spinlock later.
      if (owner != nullptr)
        return false;

      if (m_protected) {
        m_mutex.lock();
      } else {
        // Close the reader-registration race before publishing frame ownership.
        // Existing ordinary calls drain; later calls join the recursive device lock.
        m_exclusiveRequests.fetch_add(1, std::memory_order_acq_rel);
        BmvrTimedLock(m_mutex, 2);
        BmvrTimedDrain([&] { return m_unprotectedActivity.load(std::memory_order_acquire) != 0; }, 2);
        m_sourceFrameActive.store(true, std::memory_order_release);
        m_exclusiveRequests.fetch_sub(1, std::memory_order_release);
      }

      owner = this;
      depth = 1;
      FrameHoldStart() = ::bmvr::g_PerfLog ? ::bmvr::prof::Now() : 0;
      return true;
    }

    bool EndSourceFrameOwnership() {
      D3D9Multithread*& owner = SourceFrameOwner();
      uint32_t& depth = SourceFrameDepth();
      if (owner != this || depth == 0)
        return false;

      if (--depth != 0)
        return true;

      owner = nullptr;
      if (FrameHoldStart() != 0 && ::bmvr::g_PerfLog)
        ::bmvr::prof::LockHold(2, ::bmvr::prof::Now() - FrameHoldStart());
      FrameHoldStart() = 0;
      if (!m_protected)
        m_sourceFrameActive.store(false, std::memory_order_release);
      m_mutex.unlock();
      return true;
    }

  private:

    static D3D9Multithread*& SourceFrameOwner() {
      static thread_local D3D9Multithread* owner = nullptr;
      return owner;
    }

    static long long& FrameHoldStart() {
      static thread_local long long t = 0;
      return t;
    }

    static uint32_t& SourceFrameDepth() {
      static thread_local uint32_t depth = 0;
      return depth;
    }

    BOOL            m_protected;

    sync::RecursiveSpinlock m_mutex;
    std::atomic<uint32_t> m_unprotectedActivity { 0 };
    std::atomic<uint32_t> m_exclusiveRequests { 0 };
    std::atomic<bool> m_sourceFrameActive { false };

  };

}
