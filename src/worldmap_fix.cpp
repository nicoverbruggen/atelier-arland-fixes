// SPDX-License-Identifier: MIT
//
// Frame-rate-independent travel-map analog cursor movement.
//
// Totori and Meruru each have a mover that reads axes 0 and 1, folds in four
// digital directions, rotates that direction by the map heading, and adds the
// resulting normalized vector directly to the position at self+0x30. The
// position addition has no dt term. Each mover's immediate caller does receive
// the real frame dt, but the mover never consumes it. That is exactly the
// reported bug shape: a fixed distance per rendered frame.
//
// The fix rescales that step by min(dt * 60, 1), preserving the original
// behavior at 60 fps and below while making higher refresh rates cover the same
// distance per second. ARLAND_WORLDMAP_FIX=0 disables it for comparison.
//
// Totori and Meruru are runtime-confirmed at both 144 and 60 fps. Rorona's
// travel map is different: the stick steps between discrete locations, and
// runtime measurement found its selection cadence unchanged between 144 and
// 60 fps, so this subsystem deliberately installs nothing there.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "worldmap_fix.h"
#include "log.h"
#include "mem.h"
#include "util.h"

namespace atfix {

extern Log log;  // main.cpp

namespace {

bool fixEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_WORLDMAP_FIX");
    return !value || value[0] != '0';
  }();
  return enabled;
}

using Vec4 = std::array<float, 4>;
using DriverProc = bool (STDMETHODCALLTYPE*)(uintptr_t, float);
using MoveProc = bool (STDMETHODCALLTYPE*)(uintptr_t);
using PublishProc = void (STDMETHODCALLTYPE*)(uintptr_t, const Vec4*);

struct WorldMapAddrs {
  uintptr_t driver;
  uintptr_t move;
  uintptr_t publish;
  std::array<BYTE, 16> driverExpected;
  std::array<BYTE, 16> moveExpected;
};

constexpr std::array<BYTE, 16> kTotoriDriverExpected = {
  0x48, 0x89, 0x5c, 0x24, 0x18, 0x57, 0x48, 0x81,
  0xec, 0x80, 0x00, 0x00, 0x00, 0x48, 0x89, 0xb4,
};
constexpr std::array<BYTE, 16> kTotoriMoveExpected = {
  0x48, 0x8b, 0xc4, 0x53, 0x48, 0x81, 0xec, 0xc0,
  0x00, 0x00, 0x00, 0x0f, 0x29, 0x70, 0xe8, 0x0f,
};
constexpr std::array<BYTE, 16> kMeruruDriverExpected = {
  0x48, 0x8b, 0xc4, 0x57, 0x48, 0x81, 0xec, 0xb0,
  0x00, 0x00, 0x00, 0x48, 0xc7, 0x40, 0xa8, 0xfe,
};
constexpr std::array<BYTE, 16> kMeruruMoveExpected = {
  0x48, 0x8b, 0xc4, 0x53, 0x48, 0x81, 0xec, 0xb0,
  0x00, 0x00, 0x00, 0x0f, 0x29, 0x70, 0xe8, 0x0f,
};
constexpr std::array<BYTE, 16> kPublishExpected = {
  0x0f, 0x28, 0x81, 0xd0, 0x00, 0x00, 0x00, 0x0f,
  0x28, 0x0a, 0x66, 0x0f, 0x7f, 0x81, 0xb0, 0x00,
};

constexpr WorldMapAddrs kTotoriEn {
  0x2faf60, 0x2ff540, 0x2e9710,
  kTotoriDriverExpected, kTotoriMoveExpected,
};
constexpr WorldMapAddrs kTotoriMulti {
  0x518d20, 0x51d300, 0x507470,
  kTotoriDriverExpected, kTotoriMoveExpected,
};
constexpr WorldMapAddrs kMeruruEn {
  0x2556c0, 0x259ba0, 0x247bb0,
  kMeruruDriverExpected, kMeruruMoveExpected,
};
constexpr WorldMapAddrs kMeruruMulti {
  0x24a5e0, 0x24eac0, 0x23c310,
  kMeruruDriverExpected, kMeruruMoveExpected,
};

const WorldMapAddrs* addressesFor(const Game& game) {
  const bool english = game.exeBuild == BuildEnglish;
  switch (game.atlasVariant) {
    case AtlasTotori: return english ? &kTotoriEn : &kTotoriMulti;
    case AtlasLaterArland: return english ? &kMeruruEn : &kMeruruMulti;
    default: return nullptr;
  }
}

DriverProc originalDriver = nullptr;
MoveProc originalMove = nullptr;
PublishProc publishPosition = nullptr;
const WorldMapAddrs* g_addrs = nullptr;

// 0x2ff540 is the first call in 0x2faf60. Keep that caller's real dt scoped to
// the call chain instead of sharing it across threads.
thread_local float g_updateDt = 0.0f;


float distance3(const Vec4& a, const Vec4& b) {
  const float x = b[0] - a[0];
  const float y = b[1] - a[1];
  const float z = b[2] - a[2];
  return std::sqrt(x * x + y * y + z * z);
}

bool STDMETHODCALLTYPE tracedDriver(uintptr_t self, float dt) {
  const float previousDt = g_updateDt;
  g_updateDt = dt;
  const bool result = originalDriver(self, dt);
  g_updateDt = previousDt;
  return result;
}

bool STDMETHODCALLTYPE tracedMove(uintptr_t self) {
  Vec4 before{};
  const bool haveBefore = tryRead(self + 0x30, before);
  const float dt = g_updateDt;

  const bool result = originalMove(self);

  Vec4 after{};
  const bool haveAfter = tryRead(self + 0x30, after);
  const float rawStep = (haveBefore && haveAfter)
    ? distance3(before, after) : 0.0f;
  float factor = 1.0f;

  if (fixEnabled() && haveBefore && haveAfter && rawStep > 0.0f &&
      dt > 0.0f) {
    factor = std::clamp(dt * 60.0f, 0.0f, 1.0f);
    Vec4 corrected = after;
    for (size_t i = 0; i < 3; ++i)
      corrected[i] = before[i] + (after[i] - before[i]) * factor;

    // The mover owns its authoritative position at +0x30 and publishes it to
    // the render/model object at [self+0x28]. Correct both so the next frame
    // integrates from the corrected value and rendering sees the same value.
    std::memcpy(reinterpret_cast<void*>(self + 0x30),
                corrected.data(), sizeof(corrected));
    uintptr_t target = 0;
    if (tryRead(self + 0x28, target) && target && publishPosition)
      publishPosition(target, &corrected);
    after = corrected;
  }
  return result;
}

}  // namespace

bool installWorldMapFix(BYTE* base, const Game& game) {
  if (game.atlasVariant == AtlasRorona) {
    log("FIXES world_map=not_applicable");
    return false;
  }
  if (!fixEnabled()) {
    log("FIXES world_map=off");
    return false;
  }

  g_addrs = addressesFor(game);
  if (!g_addrs) {
    log("World-map cursor fix has no address row for this executable");
    return false;
  }

  auto* driver = base + g_addrs->driver;
  auto* move = base + g_addrs->move;
  auto* publish = base + g_addrs->publish;
  if (!matches(driver, g_addrs->driverExpected) ||
      !matches(move, g_addrs->moveExpected) ||
      !matches(publish, kPublishExpected)) {
    log("World-map cursor fix prologue mismatch; not installing");
    return false;
  }
  publishPosition = reinterpret_cast<PublishProc>(publish);

  // The inner hook is harmless without a live dt, so install it first. If the
  // outer hook fails, the mover remains unmodified because it has no live dt.
  const bool moveOk = installMinHookDetour(move,
    reinterpret_cast<void*>(&tracedMove),
    reinterpret_cast<void**>(&originalMove));
  const bool driverOk = moveOk && installMinHookDetour(driver,
    reinterpret_cast<void*>(&tracedDriver),
    reinterpret_cast<void**>(&originalDriver));
  log("FIXES world_map=", moveOk && driverOk ? "active" : "failed");
  if (verboseLogging())
    log("World-map cursor hooks driver_rva=0x", std::hex,
        g_addrs->driver, " move_rva=0x", g_addrs->move, std::dec,
        " move=", moveOk,
        " driver=", driverOk, " fix=", fixEnabled());
  return moveOk && driverOk;
}

}  // namespace atfix
