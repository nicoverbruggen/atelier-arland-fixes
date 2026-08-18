// SPDX-License-Identifier: MIT
//
// See pad_rescan.h for the defect and the correction.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>

#include "pad_rescan.h"
#include "config.h"
#include "game.h"
#include "hook_util.h"
#include "log.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// The wrapper returns the new pad object, or null when no device was found.
using PadCreateProc = void* (STDMETHODCALLTYPE*)(uintptr_t);

PadCreateProc originalPadCreate = nullptr;

// Steady clock rather than the frame counter on purpose: the engine's own gate
// already counts ticks, and counting them again would inherit the very property
// that makes the retry rate depend on refresh rate.
using Clock = std::chrono::steady_clock;
std::atomic<int64_t> g_nextAttemptNanos{0};

// Long enough that the enumeration cost stops mattering, short enough that
// plugging a controller in mid-session is not a noticeable wait.
constexpr int64_t kBackoffNanos = 3'000'000'000;   // 3 s

// All six builds. Each row is the CS-guarded PadCreate wrapper and a window
// proving the build has not moved under it.
//
// DERIVED FROM NAMED IMPORTS, not from a signature. `XInputGetState` comes in by
// ordinal 2 of `XINPUT1_3.dll`, its import thunk has exactly two call sites, and
// walking up from the create one gives a single confirmed caller at every hop:
//
//   thunk -> XInput pad create -> PadCreate -> the wrapper -> EnsurePad
//
// The wrapper's own two indirect calls resolve to IAT slots the import directory
// names `EnterCriticalSection` and `LeaveCriticalSection`, so "CS-guarded" is
// the binary's statement rather than a reading of the `add rcx, 0x90` shape.
// The method was calibrated on Rorona EN first, where it reproduced all five
// already-recorded addresses with no ambiguity at any hop.
//
// THE PROLOGUE IS SHARED FOR TWELVE BYTES AND THEN PER-BUILD: the four bytes
// after `mov rcx, qword ptr [rip + ...]` are that build's own displacement to
// the global holding the critical section. So the window cannot be shared the
// way the synthesis pump's can, and each row carries its own.
constexpr PadRescanTarget kRoronaEn {
  0x3f1e60,
  { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
    0xd9, 0x48, 0x8b, 0x0d, 0x48, 0x7e, 0x5c, 0x02 },
};
constexpr PadRescanTarget kTotoriEn {
  0x4d58a0,
  { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
    0xd9, 0x48, 0x8b, 0x0d, 0x78, 0xbb, 0xb2, 0x4e },
};
constexpr PadRescanTarget kMeruruEn {
  0x3eb980,
  { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
    0xd9, 0x48, 0x8b, 0x0d, 0x28, 0x0f, 0x54, 0x02 },
};
constexpr PadRescanTarget kRoronaMulti {
  0x407d30,
  { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
    0xd9, 0x48, 0x8b, 0x0d, 0x30, 0xf5, 0x5e, 0x02 },
};
constexpr PadRescanTarget kTotoriMulti {
  0x752ea0,
  { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
    0xd9, 0x48, 0x8b, 0x0d, 0xb8, 0x74, 0xc6, 0x4e },
};
constexpr PadRescanTarget kMeruruMulti {
  0x3eae80,
  { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
    0xd9, 0x48, 0x8b, 0x0d, 0xe8, 0xb5, 0x59, 0x02 },
};

const PadRescanTarget* addressesFor(const Game& game) {
  const bool english = game.exeBuild == BuildEnglish;
  switch (game.atlasVariant) {
    case AtlasRorona:      return english ? &kRoronaEn : &kRoronaMulti;
    case AtlasTotori:      return english ? &kTotoriEn : &kTotoriMulti;
    case AtlasLaterArland: return english ? &kMeruruEn : &kMeruruMulti;
    default:               return nullptr;
  }
}

bool fixEnabled() {
  return featureEnabled(Feature::PadRescanBackoff);
}

void* STDMETHODCALLTYPE tracedPadCreate(uintptr_t self) {
  const int64_t now = Clock::now().time_since_epoch().count();

  // Suppression happens before the call, which is the whole point -- the cost
  // being avoided is inside the original.
  if (fixEnabled()) {
    const int64_t next = g_nextAttemptNanos.load(std::memory_order_relaxed);
    if (next != 0 && now < next) {
      return nullptr;
    }
  }

  void* pad = originalPadCreate(self);

  // A pad was found: clear the backoff so the next disconnect is noticed at the
  // engine's own cadence rather than ours. Only a FAILED attempt arms it.
  if (pad)
    g_nextAttemptNanos.store(0, std::memory_order_relaxed);
  else
    g_nextAttemptNanos.store(now + kBackoffNanos, std::memory_order_relaxed);

  return pad;
}

}  // namespace

bool installPadRescanBackoff(BYTE* base, const Game& game) {
  if (!fixEnabled()) {
    log("FIXES pad_rescan=off");
    return false;
  }
  const PadRescanTarget* target = addressesFor(game);
  if (!base || !target) {
    log("FIXES pad_rescan=unavailable (no address row for this executable;"
        " the wrapper has not been located in the multilingual builds)");
    return false;
  }

  BYTE* wrapper = base + target->wrapperRva;
  if (!matches(wrapper, target->expected)) {
    log("FIXES pad_rescan=declined (prologue mismatch at rva=0x", std::hex,
        target->wrapperRva, std::dec, ")");
    return false;
  }

  const bool ok = installMinHookDetour(wrapper,
    reinterpret_cast<void*>(&tracedPadCreate),
    reinterpret_cast<void**>(&originalPadCreate));

  log("FIXES pad_rescan=", ok ? "active" : "failed",
      " wrapper_rva=0x", std::hex, target->wrapperRva, std::dec,
      " backoff_s=", kBackoffNanos / 1'000'000'000);
  return ok;
}

}  // namespace atfix
