// SPDX-License-Identifier: MIT
//
// Opening-movie skip.
//
// The game plays its movies through one open routine that takes the player
// object and an index into a small table of movies. The table is four entries
// of 0x20 bytes, each holding a file name, a frame size and a caption; index 0
// is `opening.wmv`, and the rest are the endings. Gating on that index is what
// keeps this feature to the intro: the ending movies go through the same
// routine and must keep working.
//
// The skip does not invent a code path. The open routine already begins by
// asking whether the movie subsystem is ready, and when the answer is no it
// writes 1 to the player's state byte and returns without opening anything.
// The per-frame movie update reads that same byte first and returns "not
// playing" immediately, so the caller advances as though the movie had
// finished. That is the engine's own graceful degradation for a movie it
// cannot play, which means the surrounding code is already written to handle
// it. The detour reproduces exactly that: state byte to 1, no original call.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>

#include "game.h"
#include "log.h"
#include "movie_skip.h"

namespace atfix {

extern Log log;  // main.cpp

namespace {

// (player, movieIndex, flag, context). Returns nothing.
using MovieOpenProc = void (STDMETHODCALLTYPE*)(uintptr_t, int32_t, BYTE,
                                                uintptr_t);

// The open routine per game and build. Each English address was anchored on
// the single reference to that build's own "Res/x64/movie" path string, not
// taken from the homology vote -- which came back WEAK for Totori, whose
// routine is a slightly different compile. The multilingual addresses are
// MATCH from their own English build, and index 0 was confirmed to be
// opening.wmv in all six movie tables.
constexpr uintptr_t kMovieOpenRvas[3][2] = {
  //             English    multilingual
  /* Rorona */ { 0x06c2d0,  0x072fe0 },
  /* Totori */ { 0x2b6a00,  0x4d4870 },
  /* Meruru */ { 0x3ce9b0,  0x3cdec0 },
};

int titleRow(Title t) {
  switch (t) {
    case Title::Rorona: return 0;
    case Title::Totori: return 1;
    case Title::Meruru: return 2;
    default: return -1;
  }
}

// Table index of opening.wmv, and the player state byte the open routine sets
// when it declines to play.
constexpr int32_t kOpeningMovieIndex = 0;
constexpr uintptr_t kPlayerStateOffset = 0x30;

// Identical between a game's two builds, but not between games: Totori's
// routine is a different compile, so it carries its own window.
constexpr std::array<BYTE, 16> kMovieOpenExpectedRoronaMeruru = {
  0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
  0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24,
};
constexpr std::array<BYTE, 16> kMovieOpenExpectedTotori = {
  0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41,
  0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24, 0x40,
};

MovieOpenProc originalMovieOpen = nullptr;

// Resolved once at install time; the hook must not read the ini or the
// environment on the thread the engine calls it from.
std::atomic<bool> skipping{false};

void STDMETHODCALLTYPE skippedMovieOpen(uintptr_t self, int32_t index,
                                        BYTE flag, uintptr_t context) {
  if (!skipping.load(std::memory_order_relaxed) ||
      index != kOpeningMovieIndex) {
    originalMovieOpen(self, index, flag, context);
    return;
  }
  *reinterpret_cast<BYTE*>(self + kPlayerStateOffset) = 1;
}

}  // namespace

bool installMovieSkip(BYTE* base, const Game& game) {
  if (featureSupport(Feature::SkipIntroMovie) == Support::Unsupported) {
    log("FIXES intro_movie_skip=not_applicable");
    return false;
  }
  if (!featureEnabled(Feature::SkipIntroMovie)) {
    log("FIXES intro_movie_skip=off");
    return false;
  }

  const int row = titleRow(currentTitle());
  if (row < 0) {
    log("FIXES intro_movie_skip=not_applicable");
    return false;
  }
  const uintptr_t rva = kMovieOpenRvas[row][
    game.exeBuild == BuildEnglish ? 0 : 1];
  BYTE* target = base + rva;
  const auto& expected = row == 1
    ? kMovieOpenExpectedTotori : kMovieOpenExpectedRoronaMeruru;
  if (!matches(target, expected)) {
    log("Intro-movie-skip prologue mismatch rva=0x", std::hex, rva, std::dec,
        "; not installing");
    return false;
  }

  const bool installed = installMinHookDetour(
    target, reinterpret_cast<void*>(&skippedMovieOpen),
    reinterpret_cast<void**>(&originalMovieOpen));
  if (installed)
    skipping.store(true, std::memory_order_relaxed);
  log("FIXES intro_movie_skip=", installed ? "active" : "failed",
      " rva=0x", std::hex, rva, std::dec);
  return installed;
}

}  // namespace atfix
