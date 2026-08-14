// SPDX-License-Identifier: MIT
//
// Field-map character jitter: the probe that identified the cause, and the four
// pieces of the fix for it. See field_physics.h.
//
// The first two are a rescale of the engine constant behind the jitter, which
// makes it mean a speed instead of a per-frame distance, and a resting
// stabilizer that holds the character still while it is genuinely at rest. The
// rescale alone keeps ground contact but leaves a small sawtooth, because
// gravity goes on integrating while resting; the stabilizer is what removes the
// motion. Between them they cover the player standing still.
//
// The other two exist because neither of those reaches a character that is
// MOVING, which is every roaming monster on a slope. The engine holds a
// character up by cancelling its vertical velocity on any frame it has ground
// contact, and it keeps the grounded flag alive for 0.0666667 seconds after
// contact is lost -- without stopping gravity for that window. So a character
// whose contact flickers free-falls while the engine still considers it
// grounded, and the whole accumulated drop is corrected in one frame when
// contact returns. Fall, fall, fall, jump, about fifteen times a second. The
// amplitude is set by a wall-clock constant, so it is the same at any frame
// rate and only its appearance changes: a fast buzz at high refresh, bumpy
// walking at 60.
//
// The ground ray is the fix, and it runs before the update integrates anything:
// cast down from the feet, and where ground is found, put the character on it
// and take its vertical velocity away. What is left is one gravity step per
// frame, about a hundred times smaller than the bounce it replaces, with
// nothing accumulating behind it. The grace hold is the weaker fallback for
// frames where the ray finds nothing.
//
// All four are on by default, each behind its own env switch that turns it off,
// so they can be A/B'd against vanilla and against each other at runtime.
//
// The probe (ARLAND_FIELD_TRACE=1) wraps the controller's per-frame update and,
// on each ground-contact change, dumps the frames either side of it. That is how
// the cause was measured, and it is kept because it is how any future build gets
// re-checked.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

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
constexpr uintptr_t kVelOffset = 0x50;        // three contiguous floats
constexpr uintptr_t kVelYOffset = 0x54;
constexpr uintptr_t kPosOffset = 0x60;
constexpr uintptr_t kPosYOffset = 0x64;
constexpr uintptr_t kEntryPosOffset = 0x70;   // pos is copied here at entry
constexpr uintptr_t kEntryPosYOffset = 0x74;
constexpr uintptr_t kFootYOffset = 0xb0;
constexpr uintptr_t kAirTimerOffset = 0xb8;
constexpr uint32_t kGroundedBit = 0x100;

// Past the last field touched here, so one range check covers the whole set.
constexpr size_t kControllerSpan = 0xbc;

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
DWORD g_thresholdProtection = 0;    // the page's protection before it was opened

// Rescaling the game's own constant keeps the full refresh rate, but it only
// reduces the movement rather than removing it, and it writes to the game's
// memory. The shipped answer is the frame-rate cap, so this stays an
// investigative switch rather than a documented option.
// On by default since 2026-07-25, when the pair was measured at a vsync-paced
// 144 fps: the rescale alone still let the cauldron interaction prompt flicker,
// and the two together held the character steady. Set the variable to 0 to run
// the game's own behaviour, which is what an A/B or a bug report wants.
bool engineFixEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_FIELD_ENGINE_FIX");
    return !value || value[0] != '0';
  }();
  return enabled;
}

// The other half of the fix, gated separately from the rescale so the two can be
// measured apart: the rescale on its own is the "does contact hold" experiment,
// the stabilizer on its own is the "is it actually still" one.
// On by default, and specifically NOT redundant with the rescale above: at 144
// fps the rescale on its own leaves the residual sawtooth this removes. Set to
// 0 to disable.
bool stabilizerEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_FIELD_STABILIZER");
    return !value || value[0] != '0';
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

// Both halves answer to one configuration key, so a single control turns the
// whole fix off and gives back the engine's own field movement. Each also keeps
// its own environment switch, for an A/B between them rather than against
// vanilla. The launcher exposes the key on its Debug page, which is only
// reachable with verbose logging on.
bool groundRayEnabled() {
  if (!fieldJitterFix())
    return false;
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_FIELD_GROUND_RAY");
    return !value || value[0] != '0';
  }();
  return enabled;
}

// The weaker half, for frames where the ray finds no ground.
bool graceHoldEnabled() {
  if (!fieldJitterFix())
    return false;
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_FIELD_GRACE_HOLD");
    return !value || value[0] != '0';
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

bool g_stabilizerActive = false;   // false unless requested and verified
bool g_stabilizerHeld = false;     // whether the last frame was actually held

// A speed low enough that nothing the player is doing produces it, but not
// exactly zero, since these components come out of float arithmetic. For scale,
// the rescaled threshold discards anything under 0.51 units/s.
constexpr float kRestSpeedEpsilon = 0.001f;

// There was an escape hatch here: every third of a second the character was
// released for one untouched frame, so that ground moving away underneath a
// held character would still be noticed. It was removed once it was traced
// through instead of reasoned about, because it could not do that.
//
// A released frame starts from vel.Y = 0, since the previous frame zeroed it,
// so the distance it produces is g*dt^2: about 0.0007 units at 144 Hz. That is
// below the rescaled threshold, and below the game's own, at every frame rate
// above roughly 29 fps. So the resolver reverts the released frame like any
// other and the ground-snap sweep, which sits on the other branch, still never
// runs. The hatch cost a frame of holding and bought nothing.
//
// The hazard it was aimed at also does not arise. Ground moving up into the
// character needs no hatch: penetration push-out is applied to the position
// before the movement is measured, so it accumulates until the frame stands on
// its own, the position no longer matches the entry copy, and the hold releases
// itself. Ground receding downward would need the snap, but no Arland field map
// has moving floors, lifts or platforms to produce it.
//
// If one is ever needed, the only form that works is to stop holding and stay
// released until a frame is not reverted, which from rest takes about five
// frames at 144 Hz and always about half the grounded grace period, at any
// frame rate, provided the rescale is active.

// The controller is a live heap object reached through a pointer the detour was
// handed, so the range is proved committed and writable before any of the
// offsets above are dereferenced. One query covers all of them, which matters
// because this runs on every field frame.
bool controllerWritable(uintptr_t self) {
  if (!self)
    return false;
  MEMORY_BASIC_INFORMATION mbi = {};
  if (!VirtualQuery(reinterpret_cast<void*>(self), &mbi, sizeof(mbi)))
    return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD))
    return false;
  const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  if (!(mbi.Protect & writable))
    return false;
  const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  return self >= base && self + kControllerSpan <= base + mbi.RegionSize;
}

// --- the ground ray ---------------------------------------------------------
//
// The engine already knows how to put a character at the right height: cast a
// ray straight down from the feet and set the position to the hit plus a small
// bias. It runs that only for a character standing still, because the gate in
// front of it demands this frame's resolved movement be under 1.1920929e-5. A
// walking character never qualifies, so its height comes from sliding along
// whatever contact planes were collected, and on a slope that is what
// oscillates.
//
// This runs the same query for a character that is moving. Ours rather than the
// engine's, for one reason: the ray length. The engine reaches 5 units below
// the feet, which is safe when the caller is stationary and already on the
// ground, and is not safe here. A character that has just walked off a ledge is
// still flagged grounded for the grace period, and a 5 unit ray would snap it
// to the ground below instead of letting it fall. A short ray finds the surface
// underfoot and nothing else, so walking off an edge simply misses and falls.
//
// On a hit the air timer is cleared as well. Holding the character at the
// surface is not enough on its own: the engine only notices ground when a
// character sinks into it, so a character held exactly on the surface never
// re-establishes contact, the grace period expires, and it drops. Clearing the
// timer keeps the grounded flag alive for as long as the ray keeps finding
// ground, and no longer -- which is what makes the ledge case work.
struct GroundRayAddrs {
  uintptr_t raycast;       // the adjustor thunk, not the ray march behind it
  uintptr_t filterVtable;  // PSSG::detail::RaycastFilter
  uintptr_t translate;     // pos += delta, and push to the scene node
};
constexpr GroundRayAddrs kGroundRayRoronaEn { 0x63b2e0, 0xb30798, 0x552dd0 };
constexpr GroundRayAddrs kGroundRayRoronaMl { 0x6511b0, 0xb5a638, 0x568ca0 };
constexpr GroundRayAddrs kGroundRayMeruruEn { 0x56e630, 0xa62ea0, 0x504ec0 };
constexpr GroundRayAddrs kGroundRayMeruruMl { 0x56db60, 0xabe090, 0x5044b0 };

// Totori is deliberately absent. Its addresses would be no harder to read out,
// but the defect was looked for there and not found, and a feature that calls
// into the game earns its place by fixing something.
const GroundRayAddrs* groundRayAddressesFor(const Game& game) {
  const bool english = game.exeBuild == BuildEnglish;
  switch (game.atlasVariant) {
    case AtlasRorona:      return english ? &kGroundRayRoronaEn
                                          : &kGroundRayRoronaMl;
    case AtlasLaterArland: return english ? &kGroundRayMeruruEn
                                          : &kGroundRayMeruruMl;
    default: return nullptr;   // Totori, and anything unrecognized
  }
}

// How far below the feet to look. Well under a character's height, so a step or
// a slope is found and a ledge is not.
constexpr float kGroundRayReach = 0.35f;
// The engine's own bias, so a character sits where the engine would put it.
constexpr float kGroundRayBias = 0.01f;

constexpr uintptr_t kQueryIfaceOffset = 0x98;  // the collision scene wrapper
constexpr uintptr_t kFootOffset = 0xb4;        // feet to capsule origin
constexpr uintptr_t kNodeOffset = 0x20;
constexpr uintptr_t kWrapperScene = 0x10;
constexpr uintptr_t kWrapperDirty = 0x18;

// The query descriptor, read out of the resolver's own construction of it at
// 0x504907..0x504ac9. Laid out as raw bytes with explicit offsets rather than
// as a struct: the engine's field order is what it is, and a compiler that
// pads differently would corrupt the call rather than fail to build.
constexpr size_t kRayDescSize = 0x70;
constexpr size_t kRayHitPos = 0x00;    // out, vec4
constexpr size_t kRayOrigin = 0x20;    // in, vec4
constexpr size_t kRayDirection = 0x30; // in, vec4, unit
constexpr size_t kRayFlag40 = 0x40;
constexpr size_t kRayDist44 = 0x44;
constexpr size_t kRayHitObject = 0x48; // out
constexpr size_t kRayFilter = 0x50;    // in, points at one qword: the vtable
constexpr size_t kRayMask = 0x58;      // in, the engine passes 3
constexpr size_t kRayDist60 = 0x60;    // in, the extent that is actually read
constexpr size_t kRayFlag64 = 0x64;
constexpr size_t kRayZero68 = 0x68;

using PFN_Raycast = void* (*)(void* scene, void* descriptor);
using PFN_Translate = void (*)(uintptr_t self, const void* delta);

bool g_groundRayActive = false;
PFN_Raycast g_raycast = nullptr;
PFN_Translate g_translate = nullptr;
uintptr_t g_filterVtable = 0;     // the whole filter object is this one pointer

// Why the ray did or did not correct, counted per reason. The first run showed
// it working where it fired and not firing often enough, and there were four
// candidate reasons with nothing to choose between them.
struct GroundRayCounts {
  uint32_t calls;      // resolver calls reaching the feature
  uint32_t notGround;  // the engine does not consider the actor grounded
  uint32_t rising;     // moving upward: not ours to correct
  uint32_t noScene;    // no wrapper, no node, or no collision scene
  uint32_t dirty;      // scene flagged dirty, frame skipped
  uint32_t missed;     // ray found nothing within reach
  uint32_t rejected;   // hit further than the ray could have reached
  uint32_t applied;
};
GroundRayCounts g_rayCounts = {};

bool applyGroundRay(uintptr_t self) {
  if (!g_groundRayActive || !controllerWritable(self))
    return false;
  ++g_rayCounts.calls;
  // Reported by call count rather than by clock: this runs inside the resolver
  // with no timer to hand, and the question it answers is a ratio, so a fixed
  // denominator reads more directly than a fixed interval would.
  if (verboseLogging() && g_rayCounts.calls % 4000 == 0) {
    log("GROUNDRAY calls=", std::dec, g_rayCounts.calls,
        " applied=", g_rayCounts.applied,
        " notGround=", g_rayCounts.notGround,
        " rising=", g_rayCounts.rising,
        " noScene=", g_rayCounts.noScene,
        " dirty=", g_rayCounts.dirty,
        " missed=", g_rayCounts.missed,
        " rejected=", g_rayCounts.rejected);
  }

  uint32_t flags = 0;
  float velY = 0.0f;
  float pos[3] = {};
  float foot = 0.0f;
  uintptr_t wrapper = 0;
  uintptr_t node = 0;
  std::memcpy(&flags, reinterpret_cast<const void*>(self + kGroundedOffset),
              sizeof(flags));
  std::memcpy(&velY, reinterpret_cast<const void*>(self + kVelYOffset),
              sizeof(velY));
  std::memcpy(pos, reinterpret_cast<const void*>(self + kPosOffset),
              sizeof(pos));
  std::memcpy(&foot, reinterpret_cast<const void*>(self + kFootOffset),
              sizeof(foot));
  std::memcpy(&wrapper, reinterpret_cast<const void*>(self + kQueryIfaceOffset),
              sizeof(wrapper));
  std::memcpy(&node, reinterpret_cast<const void*>(self + kNodeOffset),
              sizeof(node));

  // Only a character the engine still considers grounded, and only one that is
  // not rising. A jump clears the grounded bit outright, so this cannot fight
  // one, and a genuine fall has already cleared it by the time it matters.
  if ((flags & kGroundedBit) == 0) {
    ++g_rayCounts.notGround;
    return false;
  }
  // Deliberately NOT skipped when the character is rising, though it was at
  // first. Walking up a slope is exactly that case: depenetration pushes the
  // character out of the ground ahead of it, which reads as upward velocity, so
  // refusing to correct a rising character refuses the whole uphill walk and
  // hands it back to the bouncing it is meant to replace. The guard was there to
  // avoid fighting a jump, and a jump already clears the grounded bit checked
  // above, so it was protecting against something twice and costing a real case
  // once. Counted rather than acted on, because how often it happens is worth
  // knowing.
  if (velY > 0.0f)
    ++g_rayCounts.rising;
  if (!wrapper || !node) {
    ++g_rayCounts.noScene;
    return false;
  }

  uintptr_t scene = 0;
  uint8_t dirty = 1;
  if (!tryRead(wrapper + kWrapperScene, scene) ||
      !tryRead(wrapper + kWrapperDirty, dirty) || !scene) {
    ++g_rayCounts.noScene;
    return false;
  }
  // A dirty scene is rebuilt by the engine before its own queries. Rather than
  // call that rebuild, this frame is skipped: a missed correction costs one
  // frame of the defect it is fixing, and calling into a rebuild from here
  // would be the most invasive thing this feature does.
  if (dirty != 0) {
    ++g_rayCounts.dirty;
    return false;
  }

  alignas(16) uint8_t descriptor[kRayDescSize] = {};
  const float origin[4] = { pos[0], pos[1] + foot, pos[2], 0.0f };
  const float direction[4] = { 0.0f, -1.0f, 0.0f, 0.0f };
  const float reach = foot + kGroundRayReach;
  const uint64_t mask = 3;
  const uint64_t zero = 0;
  const void* filter = &g_filterVtable;
  std::memcpy(descriptor + kRayOrigin, origin, sizeof(origin));
  std::memcpy(descriptor + kRayDirection, direction, sizeof(direction));
  descriptor[kRayFlag40] = 0;
  std::memcpy(descriptor + kRayDist44, &reach, sizeof(reach));
  std::memcpy(descriptor + kRayHitObject, &zero, sizeof(zero));
  std::memcpy(descriptor + kRayFilter, &filter, sizeof(filter));
  std::memcpy(descriptor + kRayMask, &mask, sizeof(mask));
  std::memcpy(descriptor + kRayDist60, &reach, sizeof(reach));
  descriptor[kRayFlag64] = 1;
  std::memcpy(descriptor + kRayZero68, &zero, sizeof(zero));

  if (!g_raycast(reinterpret_cast<void*>(scene), descriptor)) {
    ++g_rayCounts.missed;   // nothing underfoot: a real fall, leave it alone
    return false;
  }

  float hitY = 0.0f;
  std::memcpy(&hitY, descriptor + kRayHitPos + sizeof(float), sizeof(hitY));
  const float delta = (hitY + kGroundRayBias) - pos[1];
  // The ray cannot report a surface further than its own length, so a delta
  // outside that means the hit is not what this feature thinks it is. Refusing
  // is free; a bad correction is a character teleporting.
  if (!(delta > -reach && delta < reach)) {
    ++g_rayCounts.rejected;
    return false;
  }

  const float move[4] = { 0.0f, delta, 0.0f, 0.0f };
  g_translate(self, move);
  // And take away the fall itself, not just its result. Correcting the position
  // after the update leaves the character having already been moved for this
  // frame, and the render traversal reads the transform on its own schedule --
  // so the physics can end every frame in the right place while the picture
  // still shows the bounce. Zeroing the velocity here, before the update
  // integrates it, means the frame never produces the movement at all: what is
  // left is one gravity step of g*dt^2, with nothing accumulating behind it.
  const float rest = 0.0f;
  std::memcpy(reinterpret_cast<void*>(self + kVelYOffset), &rest,
              sizeof(rest));
  // Contact is what the engine wants and the ray is what we have. Holding the
  // timer at zero keeps the grounded flag alive without needing the character
  // to sink into the ground first, which is the loop this feature exists to
  // break. It is released the moment the ray misses.
  std::memcpy(reinterpret_cast<void*>(self + kAirTimerOffset), &rest,
              sizeof(rest));
  ++g_rayCounts.applied;
  return true;
}

bool g_graceActive = false;
uint32_t g_graceHeld = 0;         // frames held, for the report

// THE GRACE HOLD. The engine keeps the grounded flag set for a fixed 0.0666667
// seconds after ground contact is lost, and gives that grace period its own
// timer at +0xb8. This holds the character's fall for the duration; it does not
// create the window, which is the engine's own. What it does not do is stop applying gravity for the duration. So a
// character inside the window is, by the engine's own bookkeeping, standing on
// the ground and falling at the same time.
//
// That is the whole of the monster vibration. Contact flickers off, the
// character free-falls for the grace period, contact returns, and the whole
// accumulated drop is corrected in one frame. Fall, fall, fall, jump. The
// measured amplitudes are what free-fall for exactly this long predicts:
// 0.0325 units against 0.033 observed at 200 fps, 0.100 against 0.10 at 60.
//
// Holding the vertical velocity at zero for those frames does not stop the
// character descending -- gravity is reapplied by the update that follows, so a
// frame in the window still drops g*dt^2. What it stops is the ACCUMULATION,
// which is the part that grows with the square of the window and produces the
// jump at the end. The residual is linear in the frame count instead: a factor
// of two at 60 fps and seven at 200, so the correction is largest exactly where
// the defect is most visible.
//
// The timer is not pinned here, unlike the resting stabilizer below. A
// character that really has walked off a ledge must still start falling when
// the grace period expires, and pinning the timer would hold it in the air.
constexpr float kGraceHoldMaxSpeed = 8.0f;

void applyGraceHold(uintptr_t self) {
  if (!g_graceActive || !controllerWritable(self))
    return;

  uint32_t flags = 0;
  float airTimer = 0.0f;
  float velY = 0.0f;
  std::memcpy(&flags, reinterpret_cast<const void*>(self + kGroundedOffset),
              sizeof(flags));
  std::memcpy(&airTimer, reinterpret_cast<const void*>(self + kAirTimerOffset),
              sizeof(airTimer));
  std::memcpy(&velY, reinterpret_cast<const void*>(self + kVelYOffset),
              sizeof(velY));

  // Grounded but with the grace timer running is exactly the window: the flag
  // is still set and the contact that set it is gone.
  if ((flags & kGroundedBit) == 0 || !(airTimer > 0.0f))
    return;
  if (!(velY < 0.0f))
    return;
  // A character launched downward by something other than gravity keeps its
  // speed. Only the gravity-sized accumulation is taken away, and by this point
  // in the window gravity alone cannot have reached this.
  if (velY < -kGraceHoldMaxSpeed)
    return;

  const float zero = 0.0f;
  std::memcpy(reinterpret_cast<void*>(self + kVelYOffset), &zero, sizeof(zero));
  ++g_graceHeld;
}

// Hold the character still while it is genuinely at rest, which is what the
// rescale on its own cannot do: gravity keeps integrating against a surface, so
// a frame still breaks through every few frames and leaves a sawtooth.
//
// Rest is three conditions at once: ground contact, no horizontal velocity, and
// a previous frame whose move the resolver threw away, which shows as the
// position sitting exactly on the copy Update took on entry. The response is to
// drop the vertical velocity gravity has been accumulating and to pin the air
// timer. Pinning the timer is the load-bearing half: held at zero the grounded
// grace period can never expire, so contact does not need a breakthrough frame
// to be re-latched. Zeroing vel.Y without it makes the jitter worse rather than
// better, because that velocity ramp is the only thing that ever clears the
// threshold.
//
// This must run BEFORE the original Update, not after it. Update refreshes the
// entry-position copy on the way in, so the same test applied after the call
// compares a value against itself, always passes, and describes nothing.
void applyRestingStabilizer(uintptr_t self, float dt) {
  g_stabilizerHeld = false;
  if (!g_stabilizerActive || !(dt > 0.0f) || !controllerWritable(self))
    return;

  uint32_t flags = 0;
  float vel[3] = {};
  float pos[3] = {};
  float entryPos[3] = {};
  std::memcpy(&flags, reinterpret_cast<const void*>(self + kGroundedOffset),
              sizeof(flags));
  std::memcpy(vel, reinterpret_cast<const void*>(self + kVelOffset), sizeof(vel));
  std::memcpy(pos, reinterpret_cast<const void*>(self + kPosOffset), sizeof(pos));
  std::memcpy(entryPos, reinterpret_cast<const void*>(self + kEntryPosOffset),
              sizeof(entryPos));

  const bool grounded = (flags & kGroundedBit) != 0;
  const bool horizontallyStill = std::fabs(vel[0]) < kRestSpeedEpsilon &&
                                 std::fabs(vel[2]) < kRestSpeedEpsilon;
  // The revert copies the entry vector back verbatim, so this is an exact
  // match rather than a near one, and a single moved component disqualifies it.
  const bool moveWasReverted = pos[0] == entryPos[0] && pos[1] == entryPos[1] &&
                               pos[2] == entryPos[2];
  if (!grounded || !horizontallyStill || !moveWasReverted)
    return;

  const float zero = 0.0f;
  std::memcpy(reinterpret_cast<void*>(self + kVelYOffset), &zero, sizeof(zero));
  std::memcpy(reinterpret_cast<void*>(self + kAirTimerOffset), &zero,
              sizeof(zero));
  g_stabilizerHeld = true;
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
  bool stabilized = false;
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
      " threshold=", g_moveThreshold ? *g_moveThreshold : kShippedThreshold,
      " held=", f.stabilized ? 1 : 0);
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
  frame.stabilized = g_stabilizerHeld;
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
  const bool tracing = fieldTraceEnabled();
  // Snapshot first, so the trace shows the state the stabilizer judged rather
  // than the state it left behind.
  const ControllerState before = tracing ? readState(self) : ControllerState{};

  // Both of these belong before the update, for different reasons: the threshold
  // so the resolver this call drives reads the value meant for this frame, the
  // stabilizer because Update overwrites the entry-position copy it tests.
  applyThreshold(dt);
  applyRestingStabilizer(self, dt);
  // Before the update, because it works by taking away the vertical velocity
  // the update is about to integrate.
  applyGraceHold(self);

  originalFieldUpdate(self, dt);

  // After it, and that placement is the whole point. Correcting the height
  // before the frame's movement sets it for where the character WAS, and the
  // update then moves it horizontally, so on a slope the height is stale by one
  // frame's step -- the character floats above the ground by the horizontal
  // step times the gradient, which reads as falling behind the terrain rather
  // than hugging it, and gets worse the faster it runs. Correcting afterwards
  // sets the height for the position the character actually ends the frame at.
  //
  // Still far ahead of the collision resolver and of the render traversal, so
  // it keeps what the earlier placement bought: the picture never shows an
  // uncorrected frame.
  applyGroundRay(self);
  if (tracing)
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
  // The page stays writable while the hook is live, because the hook rewrites
  // the threshold every field frame and re-protecting around each write would
  // cost a syscall pair per frame. It is put back if the install then fails, so
  // a declined feature never leaves a page of the game's data open with nothing
  // writing to it.
  if (!VirtualProtect(threshold, sizeof(float), PAGE_READWRITE,
                      &g_thresholdProtection)) {
    log("FIELDPHYS EngineFix declined: threshold page is not writable");
    return false;
  }
  g_moveThreshold = threshold;
  return true;
}

// Undo prepareThreshold's page opening. Only for the install-failure path.
void restoreThresholdProtection() {
  if (!g_moveThreshold || !g_thresholdProtection)
    return;
  DWORD ignored = 0;
  VirtualProtect(g_moveThreshold, sizeof(float), g_thresholdProtection,
                 &ignored);
  g_thresholdProtection = 0;
}

}  // namespace

bool installFieldPhysics(BYTE* base, const Game& game) {
  const bool wantFix = engineFixEnabled();
  // The stabilizer holds the character only while it is grounded, and without
  // the rescale that precondition can drop while the character is still
  // settling: above roughly 115 fps the grace period expires before a frame
  // moves far enough to re-establish contact. Holding the two apart is only
  // useful for an A/B, and this half of the A/B is not sound, so it is refused
  // rather than run.
  const bool wantGrace = graceHoldEnabled();
  // Meruru only, and only while its three extra addresses check out below.
  const GroundRayAddrs* rayAddrs =
    groundRayEnabled() ? groundRayAddressesFor(game) : nullptr;
  bool wantStabilizer = stabilizerEnabled();
  if (wantStabilizer && !wantFix) {
    log("FIELDPHYS stabilizer needs the threshold rescale; leaving it off");
    wantStabilizer = false;
  }
  if (!wantFix && !wantStabilizer && !wantGrace && !fieldTraceEnabled()) {
    log("FIXES field_physics=off");
    return false;
  }
  const FieldPhysicsAddrs* addrs = addressesFor(game);
  if (!addrs) {
    log("FIXES field_physics=failed (unsupported executable)");
    return false;
  }

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
  // think it is, and the stabilizer's whole premise is that same function's
  // revert, so the resolver is verified before either is used.
  if ((wantFix || wantStabilizer) &&
      !matches(base + addrs->collisionResolver, resolverExpected)) {
    log("FIELDPHYS declined: unexpected collision-resolver prologue");
    return false;
  }
  if (wantFix && !prepareThreshold(base, *addrs))
    return false;
  g_stabilizerActive = wantStabilizer;
  // Independent of the rescale: this one reads the engine's own grounded flag
  // and grace timer, neither of which the rescale touches, so it is sound on
  // its own and can be measured on its own.
  g_graceActive = wantGrace;

  // The ground ray calls into the game rather than only writing to it, so each
  // of the three entry points is checked before any of them is armed. A wrong
  // address here is a call through a pointer into the middle of a function.
  if (rayAddrs) {
    const std::array<BYTE, 8> raycastExpected = {
      0x48, 0x8b, 0x49, 0x18, 0xe9, 0x00, 0x00, 0x00,
    };
    // sub rsp,0x38 / cmp [rcx+0x20],0 / je / movss xmm0,[rcx+0x60]. Byte
    // identical in both builds, jump displacement included.
    const std::array<BYTE, 12> translateExpected = {
      0x48, 0x83, 0xec, 0x38, 0x48, 0x83, 0x79, 0x20,
      0x00, 0x74, 0x66, 0xf3,
    };
    // The thunk's tail is a relative jump whose displacement differs per build,
    // so only its four leading bytes are the signature.
    const bool thunkOk =
      std::memcmp(base + rayAddrs->raycast, raycastExpected.data(), 4) == 0 &&
      base[rayAddrs->raycast + 4] == 0xe9;
    if (!thunkOk) {
      log("FIELDPHYS ground ray declined: unexpected query thunk at 0x",
          std::hex, rayAddrs->raycast, std::dec);
    } else if (!matches(base + rayAddrs->translate, translateExpected)) {
      log("FIELDPHYS ground ray declined: unexpected translate prologue at 0x",
          std::hex, rayAddrs->translate, std::dec);
    } else {
      g_raycast = reinterpret_cast<PFN_Raycast>(base + rayAddrs->raycast);
      g_translate = reinterpret_cast<PFN_Translate>(base + rayAddrs->translate);
      g_filterVtable =
        reinterpret_cast<uintptr_t>(base + rayAddrs->filterVtable);
      g_groundRayActive = true;
    }
  }

  const bool installed = installMinHookDetour(base + addrs->update,
    reinterpret_cast<void*>(&tracedFieldUpdate),
    reinterpret_cast<void**>(&originalFieldUpdate));
  if (!installed) {
    restoreThresholdProtection();   // nothing will write it now
    g_moveThreshold = nullptr;   // never leave a rescaled value without the hook
    g_stabilizerActive = false;
    g_graceActive = false;
    g_groundRayActive = false;
  }
  log("FIXES field_physics=", installed ? "active" : "failed",
      " engine_fix=", g_moveThreshold ? 1 : 0,
      " stabilizer=", g_stabilizerActive ? 1 : 0,
      " grace_hold=", g_graceActive ? 1 : 0,
      " ground_ray=", g_groundRayActive ? 1 : 0);
  if (verboseLogging())
    log("DIAGNOSTICS field_trace=", fieldTraceEnabled() ? 1 : 0);
  return installed;
}

}  // namespace atfix
