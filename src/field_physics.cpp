// SPDX-License-Identifier: MIT
//
// Field-map character jitter: the probe that identified the cause, and an
// investigative rescale of the constant behind it. See field_physics.h.
//
// Neither is a shipped feature: the fix users get is the frame-rate cap in
// main.cpp. Both here are env-gated and inert by default.
//
// The probe (ARLAND_FIELD_TRACE=1) wraps the controller's per-frame update and,
// on each ground-contact change, dumps the frames either side of it. That is how
// the cause was measured, and it is kept because it is how any future build gets
// re-checked.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>

#include "field_physics.h"
#include "config.h"
#include "log.h"
#include "mem.h"

namespace atfix {

extern Log log;   // main.cpp

namespace {

using FieldUpdateProc = void (STDMETHODCALLTYPE*)(uintptr_t, float);
FieldUpdateProc originalFieldUpdate = nullptr;

// Offsets within the controller object. Identical in all six builds: their
// Update bodies are instruction-for-instruction the same, differing only in two
// external call targets.
constexpr uintptr_t kGroundedOffset = 0x38;   // flags word; bit 8 = ground contact
constexpr uintptr_t kVelYOffset = 0x54;
constexpr uintptr_t kPosYOffset = 0x64;
constexpr uintptr_t kEntryPosYOffset = 0x74;  // pos is copied to 0x70 at entry
constexpr uintptr_t kFootYOffset = 0xb0;
constexpr uint32_t kGroundedBit = 0x100;

// The value the games ship, compared as an exact bit pattern rather than as a
// float: 0x3c0b4396 == 0.008500000461935997. Refusing to touch anything else is
// what stops a wrong address from corrupting unrelated data.
constexpr uint32_t kShippedThresholdBits = 0x3c0b4396u;
constexpr float kShippedThreshold = 0.0085f;
constexpr float kReferenceDt = 1.0f / 60.0f;
// A floor, reached only past ~1000 fps, below which the threshold stops being
// meaningful and starts colliding with the engine's own epsilons.
constexpr float kMinThreshold = 0.0005f;

// Per-build addresses. The threshold is a float in the writable data section
// with exactly one reader — the collision resolver, at +0x5d1 — and no writer
// anywhere in the image. The resolver is verified before the threshold is
// trusted, since neither is meaningful without the other.
struct FieldPhysicsAddrs {
  uintptr_t update;
  uintptr_t collisionResolver;
  uintptr_t moveThreshold;
};

constexpr FieldPhysicsAddrs kRoronaEn    { 0x553330, 0x551f40, 0x10a85a8 };
constexpr FieldPhysicsAddrs kRoronaMulti { 0x569200, 0x567e10, 0x10e56a8 };
constexpr FieldPhysicsAddrs kTotoriEn    { 0x41bff0, 0x41ac00, 0x0ca93b8 };
constexpr FieldPhysicsAddrs kTotoriMulti { 0x6995f0, 0x698200, 0x1008968 };
constexpr FieldPhysicsAddrs kMeruruEn    { 0x5053d0, 0x504040, 0x0fa3478 };
constexpr FieldPhysicsAddrs kMeruruMulti { 0x5049c0, 0x503630, 0x1009048 };

const FieldPhysicsAddrs* addressesFor(const Game& game) {
  const bool english = game.exeBuild == BuildEnglish;
  switch (game.atlasVariant) {
    case AtlasRorona:      return english ? &kRoronaEn : &kRoronaMulti;
    case AtlasTotori:      return english ? &kTotoriEn : &kTotoriMulti;
    case AtlasLaterArland: return english ? &kMeruruEn : &kMeruruMulti;
    default: return nullptr;
  }
}

float* g_moveThreshold = nullptr;   // null unless verified and made writable

// Rescaling the game's own constant keeps the full refresh rate, but it only
// reduces the movement rather than removing it, and it writes to the game's
// memory. The shipped answer is the frame-rate cap, so this stays an
// investigative switch rather than a documented option.
bool engineFixEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_FIELD_ENGINE_FIX");
    return value && value[0] != '0';
  }();
  return enabled;
}

bool fieldTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_FIELD_TRACE");
    return value && value[0] != '0';
  }();
  return enabled;
}

// Rescale the resolver's minimum-movement threshold for this frame's duration,
// turning a per-frame distance into a constant speed (0.51 units/s). Identical
// to the shipped value at 60 fps, and clamped so a long frame never raises it
// above what the game itself uses.
void applyThreshold(float dt) {
  if (!g_moveThreshold || !(dt > 0.0f))
    return;
  float scaled = kShippedThreshold * (dt / kReferenceDt);
  if (scaled > kShippedThreshold)
    scaled = kShippedThreshold;
  if (scaled < kMinThreshold)
    scaled = kMinThreshold;
  *g_moveThreshold = scaled;
}

struct ControllerState {
  float posY = 0.0f;
  float velY = 0.0f;
  float entryPosY = 0.0f;
  float footY = 0.0f;
  uint32_t grounded = 0;
  bool valid = false;
};

ControllerState readState(uintptr_t self) {
  ControllerState state;
  state.valid = tryRead(self + kPosYOffset, state.posY) &&
                tryRead(self + kVelYOffset, state.velY) &&
                tryRead(self + kGroundedOffset, state.grounded);
  if (!state.valid)
    return state;
  tryRead(self + kEntryPosYOffset, state.entryPosY);
  tryRead(self + kFootYOffset, state.footY);
  return state;
}

// A short ring of recent frames, dumped around each contact change so the event
// is readable instead of buried in per-frame noise.
struct Frame {
  float dt = 0.0f;
  ControllerState before;
  ControllerState after;
  bool used = false;
};

constexpr size_t kRing = 6;
constexpr uint32_t kMaxWindows = 8;
Frame g_ring[kRing];
size_t g_ringHead = 0;
uint32_t g_windows = 0;
uint32_t g_pendingAfter = 0;
uint32_t g_frameIndex = 0;

void emitFrame(const Frame& f, const char* tag, uint32_t index) {
  log("FIELDPHYS ", tag, " n=", std::dec, index,
      " dt=", f.dt,
      " y=", f.before.posY, "->", f.after.posY,
      " entry_y=", f.after.entryPosY,
      " vy=", f.before.velY, "->", f.after.velY,
      " flags=0x", std::hex, f.before.grounded, "->0x", f.after.grounded,
      std::dec,
      " foot=", f.after.footY,
      " threshold=", g_moveThreshold ? *g_moveThreshold : kShippedThreshold);
}

void traceFrame(float dt, const ControllerState& before,
                const ControllerState& after) {
  if (!before.valid || !after.valid)
    return;
  const uint32_t index = ++g_frameIndex;
  Frame frame;
  frame.dt = dt;
  frame.before = before;
  frame.after = after;
  frame.used = true;

  if (g_pendingAfter) {
    --g_pendingAfter;
    emitFrame(frame, "post ", index);
    return;
  }
  const bool contactChanged =
    ((before.grounded ^ after.grounded) & kGroundedBit) != 0;
  if (contactChanged && g_windows < kMaxWindows) {
    ++g_windows;
    log("FIELDPHYS --- contact ",
        (after.grounded & kGroundedBit) ? "GAINED" : "LOST",
        " (window ", std::dec, g_windows, " of ", kMaxWindows, ") ---");
    for (size_t i = 0; i < kRing; ++i) {
      const Frame& past = g_ring[(g_ringHead + i) % kRing];
      if (past.used)
        emitFrame(past, "pre  ", 0);
    }
    emitFrame(frame, "AT   ", index);
    g_pendingAfter = kRing;
    return;
  }
  g_ring[g_ringHead] = frame;
  g_ringHead = (g_ringHead + 1) % kRing;
}

void STDMETHODCALLTYPE tracedFieldUpdate(uintptr_t self, float dt) {
  // Before the update, so the resolver it drives sees this frame's threshold.
  applyThreshold(dt);

  if (!fieldTraceEnabled()) {
    originalFieldUpdate(self, dt);
    return;
  }
  const ControllerState before = readState(self);
  originalFieldUpdate(self, dt);
  traceFrame(dt, before, readState(self));
}

// Confirm the threshold really holds the shipped value, and make its page
// writable. Section flags say the data section is writable in every build, but
// Meruru is SteamStub-wrapped, so the page is protected explicitly rather than
// assumed.
bool prepareThreshold(BYTE* base, const FieldPhysicsAddrs& addrs) {
  auto* threshold = reinterpret_cast<float*>(base + addrs.moveThreshold);
  uint32_t bits = 0;
  if (!tryRead(reinterpret_cast<uintptr_t>(threshold), bits)) {
    log("FIELDPHYS EngineFix declined: threshold is not readable");
    return false;
  }
  if (bits != kShippedThresholdBits) {
    log("FIELDPHYS EngineFix declined: expected 0x", std::hex,
        kShippedThresholdBits, " at the threshold, found 0x", bits, std::dec);
    return false;
  }
  DWORD previous = 0;
  if (!VirtualProtect(threshold, sizeof(float), PAGE_READWRITE, &previous)) {
    log("FIELDPHYS EngineFix declined: threshold page is not writable");
    return false;
  }
  g_moveThreshold = threshold;
  return true;
}

}  // namespace

bool installFieldPhysics(BYTE* base, const Game& game) {
  const bool wantFix = engineFixEnabled();
  if (!wantFix && !fieldTraceEnabled())
    return false;
  const FieldPhysicsAddrs* addrs = addressesFor(game);
  if (!addrs)
    return false;

  // One array covers all six builds for each function: no RIP displacement
  // falls inside either 16-byte window.
  const std::array<BYTE, 16> updateExpected = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x60, 0x0f, 0x29,
    0x74, 0x24, 0x50, 0x48, 0x8b, 0xd9, 0x48, 0x8b,
  };
  const std::array<BYTE, 16> resolverExpected = {
    0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0xe8,
  };
  if (!matches(base + addrs->update, updateExpected)) {
    log("FIELDPHYS declined: unexpected controller-update prologue");
    return false;
  }
  // The threshold is only meaningful if the function reading it is the one we
  // think it is, so both are checked before either is touched.
  if (wantFix && !matches(base + addrs->collisionResolver, resolverExpected)) {
    log("FIELDPHYS EngineFix declined: unexpected collision-resolver prologue");
    return false;
  }
  if (wantFix && !prepareThreshold(base, *addrs))
    return false;

  const bool installed = installMinHookDetour(base + addrs->update,
    reinterpret_cast<void*>(&tracedFieldUpdate),
    reinterpret_cast<void**>(&originalFieldUpdate));
  if (!installed)
    g_moveThreshold = nullptr;   // never leave a rescaled value without the hook
  log("FIELDPHYS installed=", installed ? 1 : 0,
      " engine_fix=", g_moveThreshold ? 1 : 0,
      " trace=", fieldTraceEnabled() ? 1 : 0);
  return installed;
}

}  // namespace atfix
