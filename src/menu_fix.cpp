// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../vendor/minhook/include/MinHook.h"

namespace {

using PathCheckProc = bool (*)(void*, void*);
using QueueDrainProc = void (*)(void*);
using RenderTextProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using AtlasLockProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using AtlasUnlockProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);

struct Game {
  const char* executable;
  DWORD textSize;
  uintptr_t pathCheckRva;
  BYTE pushedRegister;
  uintptr_t queueDrainRva;
  uintptr_t renderTextRva;
  uintptr_t atlasLockRva;
  uintptr_t atlasUnlockRva;
  uint8_t atlasVariant;
};

enum : uint8_t {
  AtlasNone,
  AtlasRorona,
  AtlasTotori,
  AtlasLaterArland,
};

constexpr Game games[] = {
  { "A11R_x64_Release_en.exe", 0x709a9c, 0x12cc70, 0x57,
    0x08d4b0, 0x5613b0, 0x3eea10, 0x3eea60, AtlasRorona },
  { "A12V_x64_Release_en.exe", 0x67da5c, 0x18b140, 0x56,
    0x038a00, 0x430bf0, 0x4c2080, 0x4c20c0, AtlasTotori },
  { "A13V_x64_Release_EN.exe", 0x61ecec, 0x1533c0, 0x57,
    0x0d6210, 0x5115d0, 0x3ea7d0, 0x3ea7f0, AtlasLaterArland },
};

PathCheckProc originalPathCheck = nullptr;
QueueDrainProc originalQueueDrain = nullptr;
RenderTextProc originalRenderText = nullptr;
AtlasLockProc originalAtlasLock = nullptr;
AtlasUnlockProc originalAtlasUnlock = nullptr;
std::once_flag initialization;
bool supportedGame = false;
std::mutex cacheMutex;
std::unordered_set<std::string> successfulPaths;

struct AtlasRead {
  uint32_t pitch = 0;
  std::vector<uint8_t> bytes;
};

std::mutex atlasMutex;
std::unordered_map<uintptr_t, AtlasRead> atlasReads;
std::atomic<uint32_t> atlasDrainDepth = { 0 };
std::atomic<bool> atlasCacheActive = { false };
thread_local uint32_t renderTextDepth = 0;
thread_local std::vector<uintptr_t> syntheticAtlasLocks;

const char* baseName(const char* path) {
  const char* back = std::strrchr(path, '\\');
  const char* forward = std::strrchr(path, '/');
  const char* slash = !back || (forward && forward > back) ? forward : back;
  return slash ? slash + 1 : path;
}

DWORD textSectionSize(HMODULE module) {
  auto* base = reinterpret_cast<BYTE*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return 0;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return 0;
  auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
    if (!std::memcmp(section->Name, ".text", 5))
      return section->Misc.VirtualSize;
  }
  return 0;
}

const char* pssgPath(const void* stringObject) {
  if (!stringObject)
    return nullptr;
  const auto* bytes = static_cast<const BYTE*>(stringObject);
  size_t capacity = 0;
  std::memcpy(&capacity, bytes + 0x18, sizeof(capacity));
  const char* path = nullptr;
  if (capacity >= 0x10)
    std::memcpy(&path, bytes, sizeof(path));
  else
    path = static_cast<const char*>(stringObject);
  if (!path)
    return nullptr;
  const char* extension = std::strrchr(path, '.');
  return extension && !_stricmp(extension, ".PSSG") ? path : nullptr;
}

bool cachedPathCheck(void* context, void* pathString) {
  const char* path = pssgPath(pathString);
  if (!path)
    return originalPathCheck(context, pathString);
  const std::string key(path);
  {
    std::lock_guard lock(cacheMutex);
    if (successfulPaths.find(key) != successfulPaths.end())
      return true;
  }
  if (!originalPathCheck(context, pathString))
    return false;
  std::lock_guard lock(cacheMutex);
  successfulPaths.insert(key);
  return true;
}

bool atlasCacheEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_ATLAS_CACHE");
    return !value || value[0] != '0';
  }();
  return enabled;
}

void cachedQueueDrain(void* manager) {
  const bool outermost =
    atlasDrainDepth.fetch_add(1, std::memory_order_acq_rel) == 0;
  if (outermost) {
    std::lock_guard lock(atlasMutex);
    atlasReads.clear();
    atlasCacheActive.store(true, std::memory_order_release);
  }

  originalQueueDrain(manager);

  if (atlasDrainDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    atlasCacheActive.store(false, std::memory_order_release);
    std::lock_guard lock(atlasMutex);
    atlasReads.clear();
  }
}

uintptr_t cachedRenderText(uintptr_t a, uintptr_t b,
                           uintptr_t c, uintptr_t d) {
  ++renderTextDepth;
  const uintptr_t result = originalRenderText(a, b, c, d);
  --renderTextDepth;
  return result;
}

uintptr_t cachedAtlasLock(uintptr_t texture, uintptr_t output,
                          uintptr_t level, uintptr_t face) {
  uint16_t width = 0;
  uint16_t height = 0;
  if (texture) {
    const auto* bytes = reinterpret_cast<const BYTE*>(texture);
    std::memcpy(&width, bytes + 0x40, sizeof(width));
    std::memcpy(&height, bytes + 0x42, sizeof(height));
  }

  const bool inCandidateScope = renderTextDepth && output && width == 512 &&
    height == 512 && atlasCacheActive.load(std::memory_order_acquire);
  const bool candidate = inCandidateScope && atlasCacheEnabled();
  if (candidate) {
    std::lock_guard lock(atlasMutex);
    const auto found = atlasReads.find(texture);
    if (found != atlasReads.end() && !found->second.bytes.empty()) {
      *reinterpret_cast<void**>(output) = found->second.bytes.data();
      syntheticAtlasLocks.push_back(texture);
      return found->second.pitch;
    }
  }

  const uintptr_t pitch = originalAtlasLock(texture, output, level, face);
  if (candidate && pitch && pitch <= 16384) {
    const void* mapped = *reinterpret_cast<void* const*>(output);
    const size_t size = size_t(pitch) * height;
    if (mapped && size <= 8 * 1024 * 1024) {
      AtlasRead entry;
      entry.pitch = uint32_t(pitch);
      entry.bytes.resize(size);
      std::memcpy(entry.bytes.data(), mapped, size);
      std::lock_guard lock(atlasMutex);
      atlasReads.emplace(texture, std::move(entry));
    }
  }
  return pitch;
}

uintptr_t cachedAtlasUnlock(uintptr_t texture, uintptr_t a,
                            uintptr_t b, uintptr_t c) {
  if (atlasCacheActive.load(std::memory_order_acquire) &&
      !syntheticAtlasLocks.empty() &&
      syntheticAtlasLocks.back() == texture) {
    syntheticAtlasLocks.pop_back();
    return 0;
  }
  return originalAtlasUnlock(texture, a, b, c);
}

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

template <size_t N>
bool matches(const BYTE* target, const std::array<BYTE, N>& expected) {
  return !std::memcmp(target, expected.data(), expected.size());
}

bool installMinHookDetour(BYTE* target, const void* replacement,
                          void** original) {
  const MH_STATUS created = MH_CreateHook(
    target, const_cast<void*>(replacement), original);
  if (created != MH_OK)
    return false;
  return MH_EnableHook(target) == MH_OK;
}

bool installAtlasCache(BYTE* base, const Game& game) {
  if (!atlasCacheEnabled() || game.atlasVariant == AtlasNone)
    return false;

  auto* queue = base + game.queueDrainRva;
  auto* render = base + game.renderTextRva;
  auto* lock = base + game.atlasLockRva;
  auto* unlock = base + game.atlasUnlockRva;
  const std::array<BYTE, 16> roronaQueueExpected = {
    0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0x88,
  };
  const std::array<BYTE, 16> laterQueueExpected = {
    0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0x98,
  };
  const std::array<BYTE, 16> totoriQueueExpected = {
    0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xb8,
  };
  const std::array<BYTE, 15> renderExpected = {
    0x48, 0x8b, 0xc4, 0x48, 0x89, 0x50, 0x10, 0x53,
    0x48, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00,
  };
  const std::array<BYTE, 15> lockExpected = {
    0x48, 0x83, 0xec, 0x38, 0x44, 0x89, 0x4c, 0x24,
    0x20, 0x45, 0x8b, 0xc8, 0x45, 0x33, 0xc0,
  };
  const std::array<BYTE, 15> roronaUnlockExpected = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
  };
  const std::array<BYTE, 14> laterUnlockExpected = {
    0x44, 0x8b, 0xc2, 0x33, 0xd2, 0xe9, 0x06, 0x00,
    0x00, 0x00, 0xcc, 0xcc, 0xcc, 0xcc,
  };
  const bool signaturesMatch = game.atlasVariant == AtlasRorona
    ? matches(queue, roronaQueueExpected) &&
      matches(unlock, roronaUnlockExpected)
    : (game.atlasVariant == AtlasTotori
      ? matches(queue, totoriQueueExpected)
      : matches(queue, laterQueueExpected)) &&
      matches(unlock, laterUnlockExpected);
  if (!signaturesMatch || !matches(render, renderExpected) ||
      !matches(lock, lockExpected))
    return false;

  /* This order keeps every partial-install outcome inert: synthetic locks
   * require the render hook and an active drain, installed last. */
  if (!installMinHookDetour(unlock,
      reinterpret_cast<void*>(&cachedAtlasUnlock),
      reinterpret_cast<void**>(&originalAtlasUnlock)))
    return false;
  if (!installMinHookDetour(lock, reinterpret_cast<void*>(&cachedAtlasLock),
      reinterpret_cast<void**>(&originalAtlasLock)))
    return false;
  if (!installMinHookDetour(render,
      reinterpret_cast<void*>(&cachedRenderText),
      reinterpret_cast<void**>(&originalRenderText)))
    return false;
  return installMinHookDetour(queue,
    reinterpret_cast<void*>(&cachedQueueDrain),
    reinterpret_cast<void**>(&originalQueueDrain));
}

void detectAndInstallGameHooks() {
  HMODULE module = GetModuleHandleW(nullptr);
  char imagePath[MAX_PATH] = {};
  if (!module || !GetModuleFileNameA(module, imagePath, sizeof(imagePath)))
    return;
  const DWORD textSize = textSectionSize(module);
  for (const auto& game : games) {
    if (_stricmp(baseName(imagePath), game.executable) || textSize != game.textSize)
      continue;
    supportedGame = true;
    const char* enabled = std::getenv("ARLAND_MENU_FIX");
    if (enabled && enabled[0] == '0')
      return;
    auto* target = reinterpret_cast<BYTE*>(module) + game.pathCheckRva;
    const std::array<BYTE, 18> expected = {
      0x40, game.pushedRegister, 0x48, 0x81, 0xec, 0xc0, 0x00, 0x00,
      0x00, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff,
    };
    if (matches(target, expected))
      installDetour(target, reinterpret_cast<void*>(&cachedPathCheck),
        expected.size(), reinterpret_cast<void**>(&originalPathCheck));
    installAtlasCache(reinterpret_cast<BYTE*>(module), game);
    return;
  }
}

} // namespace

namespace arland {

bool initializeGameHooks() {
  std::call_once(initialization, detectAndInstallGameHooks);
  return supportedGame;
}

} // namespace arland
