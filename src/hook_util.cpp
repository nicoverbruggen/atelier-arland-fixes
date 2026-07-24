// SPDX-License-Identifier: MIT
//
// Definitions for the shared hook-installation helpers declared in hook_util.h.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "hook_util.h"
#include "../vendor/minhook/include/MinHook.h"

namespace atfix {

void writeAbsoluteJump(BYTE* destination, const void* target) {
  destination[0] = 0xff;
  destination[1] = 0x25;
  std::memset(destination + 2, 0, 4);
  const uintptr_t address = reinterpret_cast<uintptr_t>(target);
  std::memcpy(destination + 6, &address, sizeof(address));
}

bool installDetour(BYTE* target, const void* replacement,
                   size_t patchSize, void** original) {
  if (patchSize < 14)
    return false;
  auto* trampoline = static_cast<BYTE*>(VirtualAlloc(
    nullptr, patchSize + 14, MEM_COMMIT | MEM_RESERVE,
    PAGE_EXECUTE_READWRITE));
  if (!trampoline)
    return false;
  std::memcpy(trampoline, target, patchSize);
  writeAbsoluteJump(trampoline + patchSize, target + patchSize);
  FlushInstructionCache(GetCurrentProcess(), trampoline, patchSize + 14);
  *original = trampoline;

  DWORD oldProtection = 0;
  if (!VirtualProtect(target, patchSize, PAGE_EXECUTE_READWRITE, &oldProtection)) {
    VirtualFree(trampoline, 0, MEM_RELEASE);
    *original = nullptr;
    return false;
  }
  writeAbsoluteJump(target, replacement);
  std::memset(target + 14, 0x90, patchSize - 14);
  FlushInstructionCache(GetCurrentProcess(), target, patchSize);
  DWORD ignored = 0;
  VirtualProtect(target, patchSize, oldProtection, &ignored);
  return true;
}

bool installMinHookDetour(BYTE* target, const void* replacement,
                          void** original) {
  const MH_STATUS created = MH_CreateHook(
    target, const_cast<void*>(replacement), original);
  if (created != MH_OK)
    return false;
  return MH_EnableHook(target) == MH_OK;
}

}  // namespace atfix
