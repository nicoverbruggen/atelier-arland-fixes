// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>

#include "framerate.h"
#include "game.h"
#include "log.h"

namespace atfix {

extern Log log;   // lives in main.cpp

bool cutscenePresentClampEnabled() {
  static const bool enabled = featureEnabled(Feature::CutsceneFramerate);
  return enabled;
}

unsigned int clampPresentInterval(unsigned int syncInterval) {
  if (!cutscenePresentClampEnabled())
    return syncInterval;
  // Log every change in the raw interval the game requests. This is the
  // diagnostic that confirms the mechanism: a cutscene that sticks at 30 fps
  // shows a 1 -> 2 transition with no matching 2 -> 1 afterwards.
  static std::atomic<unsigned int> lastRaw{0xffffffffu};
  const unsigned int prev = lastRaw.exchange(syncInterval, std::memory_order_relaxed);
  if (prev != syncInterval)
    log("Cutscene framerate: Present sync interval ", prev, " -> ", syncInterval,
      syncInterval > 1 ? " (clamping to 1)" : "");
  return syncInterval > 1 ? 1u : syncInterval;
}

}  // namespace atfix
