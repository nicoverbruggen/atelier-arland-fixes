// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace atfix {

enum class PagePatchStage : uint8_t {
  None,
  Capacity,
  ApplyProtect,
  ApplyFlush,
  ApplyVerify,
  ApplyRestoreProtection,
  RollbackProtect,
  RollbackFlush,
  RollbackVerify,
  RollbackRestoreProtection,
};

inline const char* pagePatchStageName(PagePatchStage stage) {
  switch (stage) {
    case PagePatchStage::None:                      return "none";
    case PagePatchStage::Capacity:                  return "capacity";
    case PagePatchStage::ApplyProtect:              return "apply_protect";
    case PagePatchStage::ApplyFlush:                return "apply_flush";
    case PagePatchStage::ApplyVerify:               return "apply_verify";
    case PagePatchStage::ApplyRestoreProtection:    return "apply_restore_protection";
    case PagePatchStage::RollbackProtect:           return "rollback_protect";
    case PagePatchStage::RollbackFlush:             return "rollback_flush";
    case PagePatchStage::RollbackVerify:            return "rollback_verify";
    case PagePatchStage::RollbackRestoreProtection: return "rollback_restore_protection";
  }
  return "unknown";
}

struct PagePatch {
  BYTE* address = nullptr;
  const BYTE* original = nullptr;
  size_t size = 0;
};

struct PagePatchFailure {
  PagePatchStage stage = PagePatchStage::None;
  size_t index = 0;
};

// Transactionally replaces up to five verified byte ranges with NOPs. The
// Win32 calls are constructor-injected so the exact production state machine
// can be attacked by the permanent failure harness without patching code.
class PagePatchTransaction {
public:
  using ProtectProc = BOOL (WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
  using FlushProc = BOOL (WINAPI*)(HANDLE, LPCVOID, SIZE_T);

  explicit PagePatchTransaction(
      ProtectProc protect = ::VirtualProtect,
      FlushProc flush = ::FlushInstructionCache)
    : protect_(protect), flush_(flush) {}

  ~PagePatchTransaction() {
    if (!committed_ && !rollbackAttempted_)
      rollback();
  }

  PagePatchTransaction(const PagePatchTransaction&) = delete;
  PagePatchTransaction& operator=(const PagePatchTransaction&) = delete;

  bool applyNops(const PagePatch* patches, size_t count) {
    if (!patches || !protect_ || !flush_ || count > patches_.size()) {
      failure_ = { PagePatchStage::Capacity, count };
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      patches_[i] = patches[i];
      if (!patches_[i].address || !patches_[i].original ||
          !patches_[i].size) {
        failure_ = { PagePatchStage::Capacity, i };
        rollback();
        return false;
      }
      DWORD previous = 0;
      if (!protect_(patches_[i].address, patches_[i].size,
                    PAGE_EXECUTE_READWRITE, &previous)) {
        failure_ = { PagePatchStage::ApplyProtect, i };
        rollback();
        return false;
      }
      protections_[i] = previous;
      applied_ = i + 1;
      std::memset(patches_[i].address, 0x90, patches_[i].size);
      const bool flushed = flush_(GetCurrentProcess(), patches_[i].address,
                                  patches_[i].size) != FALSE;
      bool verified = true;
      for (size_t j = 0; j < patches_[i].size; ++j)
        verified = verified && patches_[i].address[j] == 0x90;
      DWORD ignored = 0;
      const bool protectionRestored = protect_(
        patches_[i].address, patches_[i].size, previous, &ignored) != FALSE;
      if (!flushed || !verified || !protectionRestored) {
        failure_ = {
          !flushed ? PagePatchStage::ApplyFlush
                   : (!verified ? PagePatchStage::ApplyVerify
                                : PagePatchStage::ApplyRestoreProtection),
          i,
        };
        rollback();
        return false;
      }
    }
    return true;
  }

  bool rollback() {
    if (committed_)
      return false;
    if (rollbackAttempted_)
      return rollbackComplete_;
    rollbackAttempted_ = true;
    rollbackComplete_ = true;
    for (size_t reverse = applied_; reverse-- > 0;) {
      const size_t i = reverse;
      DWORD current = 0;
      if (!protect_(patches_[i].address, patches_[i].size,
                    PAGE_EXECUTE_READWRITE, &current)) {
        noteRollbackFailure(PagePatchStage::RollbackProtect, i);
        continue;
      }
      std::memcpy(patches_[i].address, patches_[i].original,
                  patches_[i].size);
      if (!flush_(GetCurrentProcess(), patches_[i].address,
                  patches_[i].size))
        noteRollbackFailure(PagePatchStage::RollbackFlush, i);
      if (std::memcmp(patches_[i].address, patches_[i].original,
                      patches_[i].size))
        noteRollbackFailure(PagePatchStage::RollbackVerify, i);
      DWORD ignored = 0;
      if (!protect_(patches_[i].address, patches_[i].size,
                    protections_[i], &ignored))
        noteRollbackFailure(PagePatchStage::RollbackRestoreProtection, i);
    }
    return rollbackComplete_;
  }

  void commit() { committed_ = true; }
  const PagePatchFailure& failure() const { return failure_; }
  const PagePatchFailure& rollbackFailure() const { return rollbackFailure_; }

private:
  void noteRollbackFailure(PagePatchStage stage, size_t index) {
    rollbackComplete_ = false;
    if (rollbackFailure_.stage == PagePatchStage::None)
      rollbackFailure_ = { stage, index };
  }

  ProtectProc protect_ = nullptr;
  FlushProc flush_ = nullptr;
  std::array<PagePatch, 5> patches_ = {};
  std::array<DWORD, 5> protections_ = {};
  size_t applied_ = 0;
  bool committed_ = false;
  bool rollbackAttempted_ = false;
  bool rollbackComplete_ = true;
  PagePatchFailure failure_ = {};
  PagePatchFailure rollbackFailure_ = {};
};

}  // namespace atfix
