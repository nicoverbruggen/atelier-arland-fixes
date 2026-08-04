// SPDX-License-Identifier: MIT
//
// Diagnostic for the save data slots view (Totori English). Off unless
// ARLAND_SAVE_MENU_PROBE=1. It measures, it does not change behaviour.
//
// What it is for. The hardcoded waits in front of the view have been removed
// and the view now opens quickly, but moving between slots still hitches. Two
// explanations are live and the existing counters cannot tell them apart:
//
//   - the row builder's own per-frame work. It runs every frame the view is up
//     and copies roughly 4 KB per row out of the preview record vector, scans
//     that vector several times, and allocates and frees a handful of strings.
//     None of that is text rendering, so the existing TEXT heartbeat is blind
//     to it.
//   - file work on selection. The record says opening the list touches no
//     files, because the boot scan parses every save once into the record
//     vector. But nobody has checked whether MOVING the cursor reaches the
//     Steam storage helpers, and a hitch on each press would fit that.
//
// So this times the builder per call and counts the storage helpers, and
// reports the worst call in each window rather than only the mean: a mean
// hides exactly the single-frame stall a hitch is made of.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdlib>

#include "game.h"
#include "log.h"
#include "save_menu_probe.h"

namespace atfix {

extern Log log;  // main.cpp

namespace {

bool probeEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_SAVE_MENU_PROBE");
    return value && value[0] != '0';
  }();
  return enabled;
}

// Totori English. The row builder is called once per frame while the view is
// up, with rcx = scene + 0x15a8. The two storage helpers are the ones every
// save primitive funnels through.
constexpr uintptr_t kRowBuilderRva = 0x2aa10;
constexpr uintptr_t kFileExistsRva = 0x27520;
constexpr uintptr_t kFileReadRva = 0x277d0;

constexpr std::array<BYTE, 16> kRowBuilderExpected = {
  0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
  0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xac, 0x24,
};
constexpr std::array<BYTE, 16> kFileExistsExpected = {
  0x4c, 0x8b, 0xdc, 0x48, 0x81, 0xec, 0x88, 0x00,
  0x00, 0x00, 0x48, 0x8b, 0x05, 0x9f, 0x54, 0xc8,
};
constexpr std::array<BYTE, 16> kFileReadExpected = {
  0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41,
  0x57, 0x48, 0x8d, 0x6c, 0x24, 0xd1, 0x48, 0x81,
};

using RowBuilderProc = void (STDMETHODCALLTYPE*)(uintptr_t);
using StorageProc = uintptr_t (STDMETHODCALLTYPE*)(uintptr_t, uintptr_t);

RowBuilderProc originalRowBuilder = nullptr;
StorageProc originalFileExists = nullptr;
StorageProc originalFileRead = nullptr;

uint64_t qpcFrequency() {
  static const uint64_t f = [] {
    LARGE_INTEGER v = {};
    QueryPerformanceFrequency(&v);
    return v.QuadPart ? static_cast<uint64_t>(v.QuadPart) : 1u;
  }();
  return f;
}
uint64_t qpcNow() {
  LARGE_INTEGER v = {};
  QueryPerformanceCounter(&v);
  return static_cast<uint64_t>(v.QuadPart);
}
uint64_t toMicros(uint64_t ticks) {
  return (ticks * 1000000ull) / qpcFrequency();
}

// Reported per window. The maximum matters more than the total here, because a
// hitch is one long frame rather than a raised average.
uint32_t builderCalls = 0;
uint64_t builderTicks = 0;
uint64_t builderWorstTicks = 0;
uint32_t existsCalls = 0;
uint32_t readCalls = 0;
uint64_t windowStart = 0;

constexpr int kWindowMs = 600;

void reportIfDue() {
  const uint64_t now = qpcNow();
  if (!windowStart) {
    windowStart = now;
    return;
  }
  if (toMicros(now - windowStart) < static_cast<uint64_t>(kWindowMs) * 1000)
    return;
  if (builderCalls) {
    log("SAVEMENU window builder_calls=", builderCalls,
        " builder_us_total=", toMicros(builderTicks),
        " builder_us_worst=", toMicros(builderWorstTicks),
        " builder_us_mean=", toMicros(builderTicks) / builderCalls,
        " steam_exists=", existsCalls,
        " steam_read=", readCalls);
  }
  builderCalls = 0;
  builderTicks = 0;
  builderWorstTicks = 0;
  existsCalls = 0;
  readCalls = 0;
  windowStart = now;
}

void STDMETHODCALLTYPE probedRowBuilder(uintptr_t self) {
  const uint64_t started = qpcNow();
  originalRowBuilder(self);
  const uint64_t elapsed = qpcNow() - started;
  ++builderCalls;
  builderTicks += elapsed;
  if (elapsed > builderWorstTicks)
    builderWorstTicks = elapsed;
  reportIfDue();
}

uintptr_t STDMETHODCALLTYPE probedFileExists(uintptr_t a, uintptr_t b) {
  ++existsCalls;
  return originalFileExists(a, b);
}

uintptr_t STDMETHODCALLTYPE probedFileRead(uintptr_t a, uintptr_t b) {
  ++readCalls;
  return originalFileRead(a, b);
}

}  // namespace

bool installSaveMenuProbe(BYTE* base, const Game& game) {
  if (!probeEnabled())
    return false;
  if (currentTitle() != Title::Totori || game.exeBuild != BuildEnglish) {
    log("FIXES save_menu_probe=not_applicable (Totori English only)");
    return false;
  }

  BYTE* builder = base + kRowBuilderRva;
  BYTE* exists = base + kFileExistsRva;
  BYTE* read = base + kFileReadRva;
  if (!matches(builder, kRowBuilderExpected) ||
      !matches(exists, kFileExistsExpected) ||
      !matches(read, kFileReadExpected)) {
    log("FIXES save_menu_probe=signature_mismatch");
    return false;
  }

  // The counters go in first so a partial install cannot report builder time
  // against storage counts that were never being collected.
  const bool countersOk =
    installMinHookDetour(exists, reinterpret_cast<void*>(&probedFileExists),
                         reinterpret_cast<void**>(&originalFileExists)) &&
    installMinHookDetour(read, reinterpret_cast<void*>(&probedFileRead),
                         reinterpret_cast<void**>(&originalFileRead));
  const bool builderOk =
    installMinHookDetour(builder, reinterpret_cast<void*>(&probedRowBuilder),
                         reinterpret_cast<void**>(&originalRowBuilder));

  log("FIXES save_menu_probe=", (countersOk && builderOk) ? "active" : "partial",
      " builder=", builderOk ? "yes" : "no",
      " storage_counters=", countersOk ? "yes" : "no");
  return builderOk;
}

}  // namespace atfix
