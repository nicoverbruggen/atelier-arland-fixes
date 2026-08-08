// SPDX-License-Identifier: MIT
//
// Opening-movie skip.
//
// The game plays its movies through one open routine that takes the player
// object and an index into a small table of movies. The table is four entries
// of 0x20 bytes, each holding a file name, a frame size and a caption; index 0
// is `opening.wmv`, and the rest are the endings.
//
// The skip does not invent a code path. The open routine already begins by
// asking whether the movie subsystem is ready, and when the answer is no it
// writes 1 to the player's state byte and returns without opening anything.
// The per-frame movie update reads that same byte first and returns "not
// playing" immediately, so the caller advances as though the movie had
// finished. That is the engine's own graceful degradation for a movie it
// cannot play, which means the surrounding code is already written to handle
// it. The detour reproduces exactly that: state byte to 1, no original call.
//
// SAVE DATA IS NOT AT RISK. Every call site of the open routine, in all six
// builds, sets the movie's gallery seen-bit with `bts` into a bitset before
// calling in, so detouring the open routine cannot cost the player an unlock.
// (Rorona EN sets it at 0x3c636d and 0x3c63b7 and calls at 0x3c6415; Totori has
// three call sites and Meruru one, all the same shape.)
//
// WHAT IS SKIPPED: the first movie the process plays, and nothing after. See
// consumeStartupMovieBudget below for why the rule counts plays instead of
// reading the index the routine is handed.
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

// The player state byte the open routine sets when it declines to play.
constexpr uintptr_t kPlayerStateOffset = 0x30;

// ---- the startup-movie budget ---------------------------------------------
//
// The skip counts plays rather than reading the index it is given, and the
// reason is the in-game Movies gallery. All three games have one -- each
// executable carries the ExtraStateMovie class, and Rorona names a `Movies`
// menu entry -- and it reaches the player through this same open routine. So a
// rule keyed on the movie's identity, skipping `opening.wmv` wherever it comes
// from, also skips it when the player deliberately picks it from the gallery,
// with nothing on screen to say why. Counting cannot make that mistake: the
// budget is spent during boot, and the gallery is always afterwards.
//
// A count rather than a time window, which was the other candidate: a window
// has to assume how long booting takes, and would either expire early on a slow
// machine or reach too far into play on a fast one. A budget assumes nothing.
//
// The failure modes stay asymmetric, which is what makes a budget of one right.
// Too small and a movie plays: the feature did not fully work, visibly and
// harmlessly. Too large and it eats one the player asked for, which is a bug
// they cannot explain. The ordinal is logged for the same reason, so a run says
// whether one was the right size rather than leaving it assumed.
//
// The Dusk mod carries the identical rule, in its src/core/util.h, for the
// identical reason. The two projects deliberately share no code, so this is a
// copy and not an include; if the rule changes in one, change it in the other.
//
// Returns true while budget remains, and consumes one.
constexpr int kStartupMovieBudget = 1;

std::atomic<int> moviePlays{0};

bool consumeStartupMovieBudget(int budget, int* playedOut) {
  const int n = moviePlays.fetch_add(1, std::memory_order_relaxed);
  if (playedOut)
    *playedOut = n + 1;
  return n < budget;
}

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

// One line per distinct index, and nothing once every index has been seen.
std::atomic<uint32_t> seenIndices{0};

void noteIndex(int32_t index, bool skipped, int ordinal) {
  if (index < 0 || index > 31)
    return;
  const uint32_t bit = 1u << index;
  const uint32_t previous = seenIndices.fetch_or(bit, std::memory_order_relaxed);
  if (previous & bit)
    return;
  log("MOVIE: open #", std::dec, ordinal, " index=", index,
      skipped ? " (skipped)" : " (played)");
}

void STDMETHODCALLTYPE skippedMovieOpen(uintptr_t self, int32_t index,
                                        BYTE flag, uintptr_t context) {
  int ordinal = 0;
  // The budget is consumed by every play, skipped or not, so a movie arriving
  // before the boot one cannot leave it unprotected.
  const bool skip = skipping.load(std::memory_order_relaxed) &&
                    consumeStartupMovieBudget(kStartupMovieBudget, &ordinal);
  noteIndex(index, skip, ordinal);
  if (!skip) {
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
