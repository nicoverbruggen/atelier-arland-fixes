// SPDX-License-Identifier: MIT
#pragma once
//
// Shared low-level hook-installation infrastructure used across the core and Phyre engine
// layers: the per-game hook descriptor and its atlas/build
// enums, the prologue-match helper, the HookTransaction class, and the single
// detour installer. The non-inline
// definitions live in hook_util.cpp.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mem.h"

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
// byte-verified in the multilingual binary. Hooks whose RVAs are
// only known for the English build stay gated on BuildEnglish.
enum : uint8_t {
  BuildEnglish,
  BuildMultilingual,
};

// True if `target`'s bytes match `expected`, a verified prologue window.
//
// The window is checked for being mapped before it is read. Callers pass
// base + an RVA out of this repository's own tables, so a wrong entry aims the
// compare at unmapped memory and would fault during install, before the gate
// that exists to decline the install has had a chance to run. A mismatch and an
// unreadable window mean the same thing here: this is not the build the row
// describes, so do not hook it.
template <size_t N>
inline bool matches(const BYTE* target, const std::array<BYTE, N>& expected) {
  if (!readableRange(reinterpret_cast<uintptr_t>(target), expected.size()))
    return false;
  return !std::memcmp(target, expected.data(), expected.size());
}

// One all-or-nothing MinHook install. Every target is created before any is
// enabled; a failed create/enable can therefore be rolled back as one owned
// set. A repeated target is accepted only when this same transaction already
// created it for the same detour (two vtables may share an implementation).
// An MH_ERROR_ALREADY_CREATED returned by MinHook is never treated as proof of
// ownership: it may belong to another feature or another module.
enum class HookTransactionStage : uint8_t {
  None,
  Capacity,
  TargetCollision,
  Create,
  Enable,
  DisableRollback,
  RemoveRollback,
};

struct HookTransactionFailure {
  HookTransactionStage stage = HookTransactionStage::None;
  int status = 0;       // MH_STATUS when the stage is a MinHook operation
  void* target = nullptr;
};

const char* hookTransactionStageName(HookTransactionStage stage);

class HookTransaction {
public:
  HookTransaction() = default;
  ~HookTransaction();
  HookTransaction(const HookTransaction&) = delete;
  HookTransaction& operator=(const HookTransaction&) = delete;

  bool create(void* target, const void* replacement, void** original);
  bool enableAll();
  void commit();
  bool rollback();

  const HookTransactionFailure& failure() const { return failure_; }
  const HookTransactionFailure& rollbackFailure() const {
    return rollbackFailure_;
  }

private:
  static constexpr size_t kMaxHooks = 64;
  static constexpr size_t kMaxPublications = 128;

  struct HookRecord {
    void* target = nullptr;
    const void* replacement = nullptr;
    void* trampoline = nullptr;
    bool enabled = false;
    bool created = false;
  };
  struct Publication {
    size_t hook = 0;
    void** slot = nullptr;
  };

  bool publish(size_t hook, void** original);
  void clearPublications(size_t hook);

  std::array<HookRecord, kMaxHooks> hooks_ = {};
  std::array<Publication, kMaxPublications> publications_ = {};
  size_t hookCount_ = 0;
  size_t publicationCount_ = 0;
  bool committed_ = false;
  bool rollbackAttempted_ = false;
  HookTransactionFailure failure_ = {};
  HookTransactionFailure rollbackFailure_ = {};
};

// MinHook-based detour (MinHook owns the trampoline). False on failure. An
// enable failure removes the created hook before returning so a later attempt
// can retry instead of colliding with this attempt's abandoned hook.
bool installMinHookDetour(BYTE* target, const void* replacement, void** original);

}  // namespace atfix
