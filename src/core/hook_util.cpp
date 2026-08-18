// SPDX-License-Identifier: MIT
//
// Definitions for the shared hook-installation helpers declared in hook_util.h.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "hook_util.h"
#include "../../vendor/minhook/include/MinHook.h"

namespace atfix {

const char* hookTransactionStageName(HookTransactionStage stage) {
  switch (stage) {
    case HookTransactionStage::None:            return "none";
    case HookTransactionStage::Capacity:        return "capacity";
    case HookTransactionStage::TargetCollision: return "target_collision";
    case HookTransactionStage::Create:          return "create";
    case HookTransactionStage::Enable:          return "enable";
    case HookTransactionStage::DisableRollback: return "disable_rollback";
    case HookTransactionStage::RemoveRollback:  return "remove_rollback";
  }
  return "unknown";
}

HookTransaction::~HookTransaction() {
  if (!committed_ && !rollbackAttempted_)
    rollback();
}

bool HookTransaction::publish(size_t hook, void** original) {
  if (!original)
    return true;
  if (publicationCount_ == publications_.size()) {
    failure_ = { HookTransactionStage::Capacity, 0, hooks_[hook].target };
    return false;
  }
  *original = hooks_[hook].trampoline;
  publications_[publicationCount_++] = { hook, original };
  return true;
}

void HookTransaction::clearPublications(size_t hook) {
  for (size_t i = 0; i < publicationCount_; ++i) {
    if (publications_[i].hook == hook && publications_[i].slot)
      *publications_[i].slot = nullptr;
  }
}

bool HookTransaction::create(void* target, const void* replacement,
                             void** original) {
  if (!target || !replacement || committed_ || rollbackAttempted_) {
    failure_ = { HookTransactionStage::TargetCollision, 0, target };
    return false;
  }
  for (size_t i = 0; i < hookCount_; ++i) {
    if (hooks_[i].target != target)
      continue;
    if (hooks_[i].replacement != replacement) {
      failure_ = { HookTransactionStage::TargetCollision, 0, target };
      return false;
    }
    return publish(i, original);
  }
  if (hookCount_ == hooks_.size()) {
    failure_ = { HookTransactionStage::Capacity, 0, target };
    return false;
  }

  HookRecord& hook = hooks_[hookCount_];
  hook.target = target;
  hook.replacement = replacement;
  const MH_STATUS status = MH_CreateHook(
    target, const_cast<void*>(replacement), &hook.trampoline);
  if (status != MH_OK) {
    failure_ = { HookTransactionStage::Create, int(status), target };
    hook = {};
    return false;
  }
  hook.created = true;
  const size_t index = hookCount_++;
  if (!publish(index, original)) {
    // The created hook is owned even though publication ran out of space; the
    // caller's normal rollback path removes it.
    return false;
  }
  return true;
}

bool HookTransaction::enableAll() {
  if (committed_ || rollbackAttempted_)
    return false;
  for (size_t i = 0; i < hookCount_; ++i) {
    const MH_STATUS status = MH_EnableHook(hooks_[i].target);
    if (status != MH_OK) {
      failure_ = { HookTransactionStage::Enable, int(status), hooks_[i].target };
      return false;
    }
    hooks_[i].enabled = true;
  }
  return true;
}

void HookTransaction::commit() {
  committed_ = true;
}

bool HookTransaction::rollback() {
  if (committed_)
    return false;
  rollbackAttempted_ = true;
  bool clean = true;

  for (size_t i = hookCount_; i-- > 0;) {
    HookRecord& hook = hooks_[i];
    if (!hook.created || !hook.enabled)
      continue;
    const MH_STATUS status = MH_DisableHook(hook.target);
    if (status == MH_OK || status == MH_ERROR_DISABLED ||
        status == MH_ERROR_NOT_CREATED) {
      hook.enabled = false;
      if (status == MH_ERROR_NOT_CREATED) {
        hook.created = false;
        clearPublications(i);
      }
    } else {
      clean = false;
      if (rollbackFailure_.stage == HookTransactionStage::None)
        rollbackFailure_ = {
          HookTransactionStage::DisableRollback, int(status), hook.target };
    }
  }

  for (size_t i = hookCount_; i-- > 0;) {
    HookRecord& hook = hooks_[i];
    if (!hook.created || hook.enabled)
      continue;
    const MH_STATUS status = MH_RemoveHook(hook.target);
    if (status == MH_OK || status == MH_ERROR_NOT_CREATED) {
      hook.created = false;
      clearPublications(i);
    } else {
      clean = false;
      if (rollbackFailure_.stage == HookTransactionStage::None)
        rollbackFailure_ = {
          HookTransactionStage::RemoveRollback, int(status), hook.target };
    }
  }
  return clean;
}

bool installMinHookDetour(BYTE* target, const void* replacement,
                          void** original) {
  HookTransaction transaction;
  if (!transaction.create(target, replacement, original) ||
      !transaction.enableAll()) {
    transaction.rollback();
    return false;
  }
  transaction.commit();
  return true;
}

}  // namespace atfix
