// SPDX-License-Identifier: MIT
#pragma once

// Guarded reads of game memory. The mod walks engine objects through raw pointers
// at reverse-engineered offsets, so every dereference must first prove the address
// is mapped -- a wild, stale, or freed pointer would otherwise fault. readableRange
// is the primitive; tryRead wraps the guard and the copy together so a multi-level
// pointer walk cannot forget a level. Forgetting a level on a freed battle object
// is exactly the class of bug behind the battle-exit crash fixed in menu_fix.cpp:
// prefer tryRead over a bare `*reinterpret_cast<T*>(addr)` for anything that reads
// engine memory.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace atfix {

// True if [p, p+n) is committed, readable, non-guard memory.
inline bool readableRange(uintptr_t p, size_t n) {
  if (!p)
    return false;
  MEMORY_BASIC_INFORMATION mbi = {};
  if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)))
    return false;
  if (mbi.State != MEM_COMMIT)
    return false;
  const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  if (!(mbi.Protect & readable) || (mbi.Protect & PAGE_GUARD))
    return false;
  const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  return p >= base && p + n <= base + mbi.RegionSize;
}

// Read a trivially-copyable T from `addr` into `out`, but only if the whole object
// is mapped. Returns false and leaves `out` untouched otherwise. A guarded walk
// then reads as a chain that cannot skip a guard, e.g. the two-level *(*(slot))
// that faulted in trackBattleStateTick:
//   uintptr_t stateObj = 0, vt = 0;
//   if (!tryRead(slot, stateObj) || !stateObj || !tryRead(stateObj, vt)) return;
template<class T>
inline bool tryRead(uintptr_t addr, T& out) {
  static_assert(std::is_trivially_copyable<T>::value,
    "tryRead copies raw bytes; T must be trivially copyable");
  if (!readableRange(addr, sizeof(T)))
    return false;
  std::memcpy(&out, reinterpret_cast<const void*>(addr), sizeof(T));
  return true;
}

}  // namespace atfix
