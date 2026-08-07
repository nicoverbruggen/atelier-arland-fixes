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

bool installMinHookDetour(BYTE* target, const void* replacement,
                          void** original) {
  const MH_STATUS created = MH_CreateHook(
    target, const_cast<void*>(replacement), original);
  if (created != MH_OK)
    return false;
  return MH_EnableHook(target) == MH_OK;
}

}  // namespace atfix
