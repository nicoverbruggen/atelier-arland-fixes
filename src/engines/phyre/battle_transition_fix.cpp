// SPDX-License-Identifier: MIT
//
// See battle_transition_fix.h for the defect, the scope, and why the correction
// takes this shape. Read it before changing anything here.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdlib>

#include "battle_transition_fix.h"
#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// meruru-en, all four. The first three are PeBattleEnter's own constructor,
// update and render; the fourth is `PSSG::PCoreKTGLRenderInterface` slot 52, the
// capture-copy enqueue the constructor calls. Why all four, in the header.
constexpr uintptr_t kCtorRva = 0x18d360;
constexpr uintptr_t kUpdateRva = 0x1901e0;
constexpr uintptr_t kRenderRva = 0x1923a0;
constexpr uintptr_t kCaptureEnqueueRva = 0x5d1d00;

// mov rax, rsp / push rsi / push rdi / push r12 / push r14 ...
constexpr std::array<BYTE, 10> kCtorExpected = {
  0x48, 0x8b, 0xc4, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41,
};
// push rbx / sub rsp, 0x30 / cmp byte ptr [rcx + 0x48], 0
constexpr std::array<BYTE, 10> kUpdateExpected = {
  0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x80, 0x79, 0x48, 0x00,
};
// mov [rsp+8], rbx / mov [rsp+0x10], rsi
constexpr std::array<BYTE, 10> kRenderExpected = {
  0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10,
};
// push rdi / sub rsp, 0x50 / mov rax, [rip+0x9d2133] / xor rax, rsp.
// The RIP displacement is inside the verified window, so this array is this
// build's alone.
constexpr std::array<BYTE, 16> kCaptureEnqueueExpected = {
  0x40, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b,
  0x05, 0x33, 0x21, 0x9d, 0x00, 0x48, 0x33, 0xc4,
};

using BattleEnterCtor = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t,
                                      uintptr_t);
using EffectUpdate = uintptr_t (*)(uintptr_t, float, float);
// FIVE arguments, and this must not be reduced. The game consumes the fourth
// from r9 and the fifth from the stack; a three-argument detour returned from
// the first render, left the next effect list entry at -1, and faulted in the
// render manager at 0x192813.
using EffectRender = void (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t,
                              uintptr_t);
using CaptureEnqueue = uint32_t (*)(uintptr_t, uintptr_t, uint32_t, uint32_t);

BattleEnterCtor originalCtor = nullptr;
EffectUpdate originalUpdate = nullptr;
EffectRender originalRender = nullptr;
CaptureEnqueue originalCaptureEnqueue = nullptr;

bool fixEnabled() {
  // Resolved once. This runs on the render thread, and hooks on that thread must
  // not touch the ini or the environment; the rule is stated in logo_skip.cpp.
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_BATTLE_TRANSITION_FIX");
    return !value || value[0] != '0';
  }();
  return enabled;
}

// All four bodies forward and do nothing else, and they must stay that way.
// Whatever the mechanism is, it is not what a detour body does: a version that
// recorded two values behaved exactly like one that recorded nothing. Anything
// added here changes a correction nobody can reason about.
uintptr_t forwardCtor(uintptr_t self, uintptr_t a2, uintptr_t a3,
                      uintptr_t a4) {
  return originalCtor(self, a2, a3, a4);
}

uintptr_t forwardUpdate(uintptr_t self, float a2, float a3) {
  return originalUpdate(self, a2, a3);
}

void forwardRender(uintptr_t self, uintptr_t a2, uintptr_t a3, uintptr_t a4,
                   uintptr_t a5) {
  originalRender(self, a2, a3, a4, a5);
}

uint32_t forwardCaptureEnqueue(uintptr_t iface, uintptr_t texture, uint32_t a3,
                               uint32_t a4) {
  return originalCaptureEnqueue(iface, texture, a3, a4);
}

}  // namespace

bool installBattleTransitionFix(BYTE* base, const Game& game) {
  if (!fixEnabled()) {
    log("FIXES battle_transition=off");
    return false;
  }
  if (!base || currentTitle() != Title::Meruru ||
      game.exeBuild != BuildEnglish) {
    log("FIXES battle_transition=unavailable"
      " (only the Meruru English build is mapped)");
    return false;
  }

  BYTE* ctor = base + kCtorRva;
  BYTE* update = base + kUpdateRva;
  BYTE* render = base + kRenderRva;
  BYTE* enqueue = base + kCaptureEnqueueRva;
  if (!matches(ctor, kCtorExpected) ||
      !matches(update, kUpdateExpected) ||
      !matches(render, kRenderExpected) ||
      !matches(enqueue, kCaptureEnqueueExpected)) {
    log("FIXES battle_transition=declined"
      " (bytes differ; this Meruru build is not the mapped one)");
    return false;
  }

  // One transaction for all four. A partial install is the worst outcome here:
  // the four have never been tested separately, so three of them is a
  // configuration nobody has tried rather than a weaker version of this one.
  HookTransaction transaction;
  const bool created =
    transaction.create(ctor, reinterpret_cast<void*>(&forwardCtor),
      reinterpret_cast<void**>(&originalCtor)) &&
    transaction.create(update, reinterpret_cast<void*>(&forwardUpdate),
      reinterpret_cast<void**>(&originalUpdate)) &&
    transaction.create(render, reinterpret_cast<void*>(&forwardRender),
      reinterpret_cast<void**>(&originalRender)) &&
    transaction.create(enqueue, reinterpret_cast<void*>(&forwardCaptureEnqueue),
      reinterpret_cast<void**>(&originalCaptureEnqueue));
  if (!created || !transaction.enableAll()) {
    const HookTransactionFailure& failure = transaction.failure();
    transaction.rollback();
    log("FIXES battle_transition=failed (stage=",
      hookTransactionStageName(failure.stage), " status=", failure.status, ")");
    return false;
  }
  transaction.commit();
  log("FIXES battle_transition=active ctor=0x", std::hex, kCtorRva,
    " update=0x", kUpdateRva, " render=0x", kRenderRva,
    " enqueue=0x", kCaptureEnqueueRva, std::dec);
  return true;
}

}  // namespace atfix
