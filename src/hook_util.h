// SPDX-License-Identifier: MIT
#pragma once
//
// Shared low-level hook-installation infrastructure used by both menu_fix.cpp and
// battle_shadow_restore.cpp: the per-game hook descriptor and its atlas/build
// enums, the prologue-match helper, and the two detour installers. The non-inline
// definitions live in hook_util.cpp.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace atfix {

// Per-game hook descriptor: the executable identity (name, .text size, the
// register pushed by the path-check prologue) and the RVAs of the hooked engine
// functions, plus which atlas variant and language build this row describes.
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
  uint8_t exeBuild;
};

enum : uint8_t {
  AtlasNone,
  AtlasRorona,
  AtlasTotori,
  AtlasLaterArland,
};

// Each game ships two executables: the English build (launcher Language=2) and
// the multilingual build (Japanese and both Chinese locales). They are separate
// compiles with distinct RVAs; the multilingual entries were located by static
// homologue matching against the English build and every hooked prologue
// byte-verified in the multilingual binary (REPORT §31). Hooks whose RVAs are
// only known for the English build stay gated on BuildEnglish.
enum : uint8_t {
  BuildEnglish,
  BuildMultilingual,
};

// True if `target`'s bytes match `expected`, a verified prologue window.
template <size_t N>
inline bool matches(const BYTE* target, const std::array<BYTE, N>& expected) {
  return !std::memcmp(target, expected.data(), expected.size());
}

// Write a 14-byte absolute jmp to `target` at `destination`.
void writeAbsoluteJump(BYTE* destination, const void* target);

// Byte-patch detour: copies the prologue to a trampoline and writes an absolute
// jump over `target` (requires patchSize >= 14). Returns the trampoline in
// `original`. False on failure.
bool installDetour(BYTE* target, const void* replacement,
                   size_t patchSize, void** original);

// MinHook-based detour (MinHook owns the trampoline). False on failure.
bool installMinHookDetour(BYTE* target, const void* replacement, void** original);

}  // namespace atfix
