// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>

#include "module_lifetime.h"

namespace atfix {

bool retainModuleForProcessLifetime() {
  static std::atomic<bool> retained{false};
  if (retained.load(std::memory_order_acquire))
    return true;

  HMODULE module = nullptr;
  if (!GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&retainModuleForProcessLifetime), &module))
    return false;

  retained.store(true, std::memory_order_release);
  return true;
}

}  // namespace atfix
