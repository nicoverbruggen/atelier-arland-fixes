// SPDX-License-Identifier: MIT
//
// Startup logo skip.
//
// The boot logos are not part of the title-screen state machine. They belong to
// ThreadEasyRenderLogo, a small object the application creates before it starts
// initialising the engine. Its update runs on the render thread and steps a six
// phase sequence over three fullscreen picture layers (warning text, Koei
// Tecmo, Gust; the English builds start at the second and never show the
// warning). Each logo costs a half-second fade in, two seconds of hold and a
// half-second fade out, so an English boot spends about six seconds here.
//
// The application does not wait for the logos before loading. It creates the
// object, performs the whole engine and resource initialisation while the
// render thread animates, and only then spins until the sequence reports its
// terminal phase. A separate title-side player blocks on the same object for
// the attract replay after an idle title screen. Both wait on nothing but the
// phase field, so writing the terminal phase releases both.
//
// Two hooks, because one is not enough to guarantee a clean screen. Forcing the
// phase stops the sequence advancing, but the picture layers are already
// constructed and their alpha only reaches the material when the layer's own
// update runs, which the forced path no longer calls. Rather than reason about
// what a never-ticked layer draws, the draw is suppressed as well. The result
// is the untouched clear colour for as long as loading genuinely takes, which
// is the honest presentation: skipping the logos does not make loading faster.
//
// The object is left structurally intact, so the game's own destructor still
// frees the picture layers.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>

#include "game.h"
#include "log.h"
#include "logo_skip.h"

namespace atfix {

extern Log log;  // main.cpp

namespace {

// ThreadEasyRenderLogo::update(float) and ::draw(context), vtable slots 1 and
// 2. Both return a BYTE-wide true in every path.
using LogoUpdateProc = BYTE (STDMETHODCALLTYPE*)(uintptr_t, float);
using LogoDrawProc = BYTE (STDMETHODCALLTYPE*)(uintptr_t, uintptr_t);

// Vtable slots 1 and 2 of ThreadEasyRenderLogo, per game and build. Each was
// taken from that game's own RTTI vtable rather than ported by homology, and
// the prologues of all twelve match the two windows below.
struct LogoRvas {
  uintptr_t update;
  uintptr_t draw;
};
constexpr LogoRvas kLogoRvas[3][2] = {
  // English                 multilingual
  /* Rorona */ { { 0x21ba30, 0x21bc90 }, { 0x2277e0, 0x227a40 } },
  /* Totori */ { { 0x277e00, 0x278070 }, { 0x494b80, 0x494df0 } },
  /* Meruru */ { { 0x1f5f00, 0x1f6170 }, { 0x1e6ad0, 0x1e6d40 } },
};

int titleRow(Title t) {
  switch (t) {
    case Title::Rorona: return 0;
    case Title::Totori: return 1;
    case Title::Meruru: return 2;
    default: return -1;
  }
}

// The phase field the two waiters poll, and the value that ends the sequence.
// Phase 5 is the terminal state: the original's own dispatch falls through it
// without touching anything.
constexpr uintptr_t kPhaseOffset = 0x20;
constexpr int32_t kPhaseFinished = 5;

// Byte-identical in the English and multilingual executables.
constexpr std::array<BYTE, 16> kLogoUpdateExpected = {
  0x48, 0x8b, 0xc4, 0x57, 0x48, 0x83, 0xec, 0x60,
  0x48, 0xc7, 0x40, 0xc8, 0xfe, 0xff, 0xff, 0xff,
};
constexpr std::array<BYTE, 16> kLogoDrawExpected = {
  0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
  0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20, 0x48,
};

LogoUpdateProc originalLogoUpdate = nullptr;
LogoDrawProc originalLogoDraw = nullptr;

// Resolved once at install time. The hooks run on the render thread, so they
// must not touch the ini or the environment; reading a plain flag keeps them
// free of both.
std::atomic<bool> skipping{false};

BYTE STDMETHODCALLTYPE skippedLogoUpdate(uintptr_t self, float elapsed) {
  if (!skipping.load(std::memory_order_relaxed))
    return originalLogoUpdate(self, elapsed);
  *reinterpret_cast<int32_t*>(self + kPhaseOffset) = kPhaseFinished;
  return 1;
}

BYTE STDMETHODCALLTYPE skippedLogoDraw(uintptr_t self, uintptr_t context) {
  if (!skipping.load(std::memory_order_relaxed))
    return originalLogoDraw(self, context);
  return 1;
}

}  // namespace

bool installLogoSkip(BYTE* base, const Game& game) {
  if (featureSupport(Feature::SkipStartupLogos) == Support::Unsupported) {
    log("FIXES logo_skip=not_applicable");
    return false;
  }
  if (!featureEnabled(Feature::SkipStartupLogos)) {
    log("FIXES logo_skip=off");
    return false;
  }

  const int row = titleRow(currentTitle());
  if (row < 0) {
    log("FIXES logo_skip=not_applicable");
    return false;
  }
  const LogoRvas& rvas = kLogoRvas[row][
    game.exeBuild == BuildEnglish ? 0 : 1];
  const uintptr_t updateRva = rvas.update;
  const uintptr_t drawRva = rvas.draw;
  BYTE* updateTarget = base + updateRva;
  BYTE* drawTarget = base + drawRva;

  if (!matches(updateTarget, kLogoUpdateExpected) ||
      !matches(drawTarget, kLogoDrawExpected)) {
    log("Logo-skip prologue mismatch update=0x", std::hex, updateRva,
        " draw=0x", drawRva, std::dec, "; not installing");
    return false;
  }

  // Install the draw suppression first: on a partial install the sequence still
  // runs and still draws, which is the shipped behaviour, rather than a stopped
  // sequence whose stale picture layers are left on screen.
  if (!installMinHookDetour(drawTarget, reinterpret_cast<void*>(&skippedLogoDraw),
                            reinterpret_cast<void**>(&originalLogoDraw))) {
    log("FIXES logo_skip=failed (draw)");
    return false;
  }
  if (!installMinHookDetour(updateTarget, reinterpret_cast<void*>(&skippedLogoUpdate),
                            reinterpret_cast<void**>(&originalLogoUpdate))) {
    log("FIXES logo_skip=failed (update)");
    return false;
  }

  skipping.store(true, std::memory_order_relaxed);
  log("FIXES logo_skip=active update=0x", std::hex, updateRva,
      " draw=0x", drawRva, std::dec);
  return true;
}

}  // namespace atfix
