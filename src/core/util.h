// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <mutex>

// Portable caller return address, used by the hook functions to recover the
// game call site (its RVA distinguishes call sites of a shared hooked routine).
// Must be a macro: wrapping the intrinsic in a function would return the
// wrapper's own return address. Returns void*; call sites reinterpret_cast it to
// uintptr_t. The enclosing function must not be inlined, which holds because the
// hook targets are address-taken (passed to MinHook).
#if defined(_MSC_VER)
  #include <intrin.h>
  #pragma intrinsic(_ReturnAddress)
  #define arlandReturnAddress() (_ReturnAddress())
#else
  #define arlandReturnAddress() (__builtin_return_address(0))
#endif

namespace atfix {

/**
 * \brief SRW-based mutex implementation
 *
 * Drop-in replacement for \c std::mutex that uses Win32
 * SRW locks, which are implemented with \c futex in wine.
 */
class mutex {

public:

  using native_handle_type = PSRWLOCK;

  mutex() { }

  mutex(const mutex&) = delete;
  mutex& operator = (const mutex&) = delete;

  void lock() {
    AcquireSRWLockExclusive(&m_lock);
  }

  void unlock() {
    ReleaseSRWLockExclusive(&m_lock);
  }

  bool try_lock() {
    return TryAcquireSRWLockExclusive(&m_lock);
  }

  native_handle_type native_handle() {
    return &m_lock;
  }

private:

  SRWLOCK m_lock = SRWLOCK_INIT;

};


}
