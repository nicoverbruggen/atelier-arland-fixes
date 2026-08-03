// SPDX-License-Identifier: MIT
//
// See mix_card.h for the defect and the correction.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "game.h"
#include "log.h"
#include "mem.h"
#include "mix_card.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

// bool __fastcall Card::Update(Card* self, float dt). The return is `mov al, 1`
// on the single exit -- it is always true.
using CardUpdateProc = bool (STDMETHODCALLTYPE*)(uintptr_t, float);

CardUpdateProc originalCardUpdate = nullptr;

// The accumulator the pump reads, adds dt to, and writes back.
constexpr uintptr_t kAccumulator = 0x820;

// One row per game, English then multilingual. Each was located by the shape of
// the defect rather than by homology: a scan for a truncation-to-int of a
// multiply by a frame rate, then whether a conditional branch separates that
// conversion from the loop top. It returns exactly one bottom-tested pump per
// executable, at these six addresses, each with the same five-byte alignment
// NOP where the pre-test belongs.
constexpr uintptr_t kCardUpdateRvas[3][2] = {
  //             English    multilingual
  /* Rorona */ { 0x293dc0,  0x2a4440 },
  /* Totori */ { 0x334b30,  0x553860 },
  /* Meruru */ { 0x275dd0,  0x26c650 },
};

// Byte-identical in all six executables, and in the four Dusk builds besides.
constexpr std::array<BYTE, 16> kCardUpdateExpected = {
  0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
  0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0xf3,
};

// The engine's own constants, as exact bit patterns rather than decimal
// literals: the predicate is a truncation, so a compiler that rounded these
// differently would decide a tick was due on a different frame than the
// original does. Verified byte-for-byte in every build.
//   0x3C88AB86 = 0.016683351f  (1/59.94, the authored step)
//   0x426FC28F = 59.939999f    (the authored rate)
float bitsToFloat(uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

const float kRate = bitsToFloat(0x426FC28Fu);

// A guard against inheriting drift rather than a behaviour change. Running
// unfixed above 60 Hz walks the accumulator unboundedly negative, so a session
// that enables this mid-run could otherwise sit below the tick threshold for a
// long time and appear frozen. Normal operation keeps the value within one step
// of zero; anything a whole second in the past is not recoverable state.
constexpr float kDriftFloor = -1.0f;

std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_ticked{0};
std::atomic<uint64_t> g_skipped{0};
std::atomic<uint64_t> g_healed{0};

int titleRow(Title t) {
  switch (t) {
    case Title::Rorona: return 0;
    case Title::Totori: return 1;
    case Title::Meruru: return 2;
    default: return -1;
  }
}

bool fixEnabled() {
  return featureEnabled(Feature::SynthesisAnimationRate);
}

bool probeEnabled() {
  const char* value = std::getenv("ARLAND_MIXCARD_PROBE");
  return value && value[0] != '0';
}

bool STDMETHODCALLTYPE correctedCardUpdate(uintptr_t self, float dt) {
  if (!fixEnabled() || !self)
    return originalCardUpdate(self, dt);

  float accumulated = 0.0f;
  if (!tryRead(self + kAccumulator, accumulated))
    return originalCardUpdate(self, dt);   // unreadable: leave the engine alone

  g_calls.fetch_add(1, std::memory_order_relaxed);

  if (accumulated < kDriftFloor) {
    accumulated = 0.0f;
    g_healed.fetch_add(1, std::memory_order_relaxed);
  }

  const float next = accumulated + dt;

  // The engine's own predicate, evaluated the way the engine evaluates it. When
  // a tick is due the original runs completely untouched -- it re-reads the same
  // field, recomputes the same count, and behaves bit-for-bit as shipped.
  if (static_cast<int>(next * kRate) >= 1) {
    g_ticked.fetch_add(1, std::memory_order_relaxed);
    return originalCardUpdate(self, dt);
  }

  // No tick this frame. Bank the elapsed time so it is not lost; the next frame
  // that crosses the threshold spends it.
  if (readableRange(self + kAccumulator, sizeof(next))) {
    std::memcpy(reinterpret_cast<void*>(self + kAccumulator), &next,
                sizeof(next));
  }
  g_skipped.fetch_add(1, std::memory_order_relaxed);
  return true;   // the original's only exit is `mov al, 1`
}

}  // namespace

bool installMixCardFix(BYTE* base, const Game& game) {
  if (featureSupport(Feature::SynthesisAnimationRate) ==
      Support::Unsupported) {
    log("FIXES synthesis_rate=not_applicable");
    return false;
  }
  if (!fixEnabled()) {
    log("FIXES synthesis_rate=off");
    return false;
  }
  const int row = titleRow(currentTitle());
  if (row < 0 || !base) {
    log("FIXES synthesis_rate=unavailable (no address row for this executable)");
    return false;
  }

  const uintptr_t rva =
    kCardUpdateRvas[row][game.exeBuild == BuildEnglish ? 0 : 1];
  BYTE* update = base + rva;
  if (!matches(update, kCardUpdateExpected)) {
    log("FIXES synthesis_rate=declined (prologue mismatch at rva=0x", std::hex,
        rva, std::dec, ")");
    return false;
  }

  const bool ok = installMinHookDetour(update,
    reinterpret_cast<void*>(&correctedCardUpdate),
    reinterpret_cast<void**>(&originalCardUpdate));

  log("FIXES synthesis_rate=", ok ? "active" : "failed",
      " update_rva=0x", std::hex, rva, std::dec,
      " probe=", probeEnabled() ? 1 : 0);
  return ok;
}

// Reported from the frame tick so a run can be judged without a debugger. The
// whole measurement is ticks-per-second against frames-per-second: the first
// should stay near 59.9 -- or 119.9 while the synthesis state is running, since
// the container is pumped twice per frame there -- and the second should scale
// with refresh rate.
void mixCardReport() {
  if (!probeEnabled())
    return;
  static uint32_t frames = 0;
  if (++frames < 120)
    return;
  frames = 0;
  const uint64_t calls = g_calls.load(std::memory_order_relaxed);
  if (!calls)
    return;
  log("MIXCARD calls=", std::dec, calls,
      " ticked=", g_ticked.load(std::memory_order_relaxed),
      " skipped=", g_skipped.load(std::memory_order_relaxed),
      " drift_heals=", g_healed.load(std::memory_order_relaxed));
}

}  // namespace atfix
