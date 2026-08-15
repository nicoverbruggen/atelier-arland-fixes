// SPDX-License-Identifier: MIT
//
// Field-map character movement: the addresses, and how each correction is
// applied.
//
// See field_physics.h for what the two defects are, what each correction does
// about them, why the ground ray runs after the engine's update rather than
// before, and which switch turns which one off. None of that is repeated here.
//
// What is here and not there: the per-build address packs, the prologue windows
// each one is verified against, the query descriptor read out of the engine's
// own construction of it, and the reasons the guards are shaped the way they
// are.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "field_physics.h"
#include "../../core/config.h"
#include "../../core/game.h"
#include "../../core/log.h"
#include "../../core/mem.h"

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
// with exactly one reader, the collision resolver, and no writer anywhere in
// the image. The resolver is verified before the threshold is trusted, since
// neither is meaningful without the other.
// The conversation state's per-frame update. Rorona and Meruru call the class
// `nspFM::clsFMStateTalk` and put it in vtable slot 2; Totori calls it
// `FieldMapStateCharaTalk` and puts it in slot 6, which is why the two carry
// different prologues below.
//
// Totori's slots were identified by what they do rather than by position: slot
// 3 allocates a 0x138 object and stores it on the state, slot 4 destroys that
// object and nulls the pointer, so those are enter and leave. Slot 6 delegates
// to a sub-object and returns true, which is the per-frame tick. It takes only
// `this` where the others also take a frame delta, and passing an extra float
// in xmm1 to a function that never reads it costs nothing, so one detour
// signature covers both shapes.
struct FieldPhysicsAddrs {
  uintptr_t update;
  uintptr_t collisionResolver;
  uintptr_t moveThreshold;
  uintptr_t talkUpdate;
};

constexpr FieldPhysicsAddrs kRoronaEn    { 0x553330, 0x551f40, 0x10a85a8, 0x368040 };
constexpr FieldPhysicsAddrs kRoronaMulti { 0x569200, 0x567e10, 0x10e56a8, 0x37d610 };
constexpr FieldPhysicsAddrs kTotoriEn    { 0x41bff0, 0x41ac00, 0x0ca93b8, 0x0566b0 };
constexpr FieldPhysicsAddrs kTotoriMulti { 0x6995f0, 0x698200, 0x1008968, 0x272d30 };
constexpr FieldPhysicsAddrs kMeruruEn    { 0x5053d0, 0x504040, 0x0fa3478, 0x34c4f0 };
constexpr FieldPhysicsAddrs kMeruruMulti { 0x5049c0, 0x503630, 0x1009048, 0x348b60 };

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

// NOT made obsolete by the ground ray, though its sibling the resting
// stabilizer was. This one is what lets a character MOVE at high frame rates:
// the resolver discards any frame whose total movement is under 0.0085, and
// ordinary walking stops clearing that distance somewhere above 600 fps, at
// which point every step is reverted and the character cannot walk at all. The
// ray corrects height and cannot help there, because the revert restores the
// whole position vector including the correction.
// Set to 0 to run the game's own behaviour, which is what an A/B wants.
bool engineFixEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_FIELD_ENGINE_FIX");
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

// Confirm the threshold really holds the shipped value, and make its page
// writable. Section flags say the data section is writable in every build, but
// Meruru is SteamStub-wrapped, so the page is protected explicitly rather than
// assumed. The page stays writable while the hook is live, because the hook
// rewrites the threshold every field frame and re-protecting around each write
// would cost a syscall pair per frame.
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
constexpr GroundRayAddrs kGroundRayTotoriEn { 0x43b950, 0x8431e8, 0x41ba90 };
constexpr GroundRayAddrs kGroundRayTotoriMl { 0x6b8f50, 0xb8ad88, 0x699090 };
constexpr GroundRayAddrs kGroundRayMeruruEn { 0x56e630, 0xa62ea0, 0x504ec0 };
constexpr GroundRayAddrs kGroundRayMeruruMl { 0x56db60, 0xabe090, 0x5044b0 };

// All three games. Totori was left out at first because the defect had been
// looked for there and not seen, which was the wrong test: its resolver is the
// same size to the byte, with the same call structure and the same three entry
// points, so it has the same defect whether or not anyone has walked a map
// steep enough to show it.
const GroundRayAddrs* groundRayAddressesFor(const Game& game) {
  const bool english = game.exeBuild == BuildEnglish;
  switch (game.atlasVariant) {
    case AtlasRorona:      return english ? &kGroundRayRoronaEn
                                          : &kGroundRayRoronaMl;
    case AtlasTotori:      return english ? &kGroundRayTotoriEn
                                          : &kGroundRayTotoriMl;
    case AtlasLaterArland: return english ? &kGroundRayMeruruEn
                                          : &kGroundRayMeruruMl;
    default: return nullptr;
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
// timer at +0xb8. What it does not do is stop applying gravity for that window.
// So a character inside it is, by the engine's own bookkeeping, standing on the
// ground and falling at the same time.
//
// This holds the fall for the duration of the window. It does not create the
// window, which is the engine's own.
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
// The timer is deliberately NOT pinned here. A character that really has walked
// off a ledge must still start falling when the grace period expires, and
// pinning the timer would hold it in the air. The ground ray does pin it, and
// can afford to: it only holds the timer down while it is actually finding
// ground underfoot, and releases it the moment the ray misses.
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

// ---- conversation anchor hold ----------------------------------------------
//
// A character the player has walked into shimmers while a conversation is open.
// Its scene node has two writers that disagree once a frame: the actor writes
// the whole local transform, which is the conversation anchor and where the
// character is meant to stand, and the controller update writes the translation
// from the controller's own position and then reads its position back out of
// the node. Whichever wrote last is drawn, so they alternate at the refresh
// rate.
//
// Diagnosed on Ayesha, where the two writers were caught with a page-protection
// write watch on the node. The same repair is ported here because the engine is
// the same: the node layout is identical -- the translation write
// `movdqa [r9+0x140], xmm1` appears exactly once in all six Arland builds and
// in Ayesha -- and the controller carries its node at the same +0x20.
//
// The anchor wins, because a character in a conversation stands where the
// conversation puts them. So the node is put back to what it held before the
// controller update ran, and the controller's own copy follows it.
// BOTH transforms, local first: PSSG::PNode keeps local at 0xd0..0x10f with its
// translation row at +0x100, and world at 0x110..0x14f with its row at +0x140.
// World is DERIVED from local, and two recomputes exist -- the controller
// update's own, and the render traversal's, which is stamp-gated and runs later
// in the frame. Restoring world alone therefore holds only while nothing bumps
// the stamp; the render path would rebuild it from a local this never touched.
// Covering the source as well as the derived value removes that dependency.
// See atelier-re-tools/systems/phyre-field-movement.md, "Position storage".
constexpr uintptr_t kNodeMatrixOffset = 0xd0;
constexpr size_t kNodeMatrixSize = 128;

std::atomic<uint64_t> g_talkSeenMs{0};

bool talkAnchorEnabled() {
  static const bool on = [] {
    const bool wanted =
      featureSupport(Feature::TalkAnchorHold) != Support::Unsupported &&
      featureEnabled(Feature::TalkAnchorHold);
    log("FIXES talk_anchor=", wanted ? "on" : "off");
    return wanted;
  }();
  return on;
}

// A timestamp rather than a flag, because the order of the talk update and the
// controller update within a frame is not known and a stale flag would either
// miss the opening frames or hold past the last.
bool conversationOnScreen() {
  const uint64_t seen = g_talkSeenMs.load(std::memory_order_relaxed);
  return seen != 0 && GetTickCount64() - seen <= 100;
}

using TalkUpdateProc = BYTE (STDMETHODCALLTYPE*)(uintptr_t, float);
TalkUpdateProc originalTalkUpdate = nullptr;

BYTE STDMETHODCALLTYPE tracedTalkUpdate(uintptr_t self, float dt) {
  g_talkSeenMs.store(GetTickCount64(), std::memory_order_relaxed);
  return originalTalkUpdate(self, dt);
}

// Byte-identical in Rorona and Meruru, both builds each, and to Ayesha's.
constexpr std::array<BYTE, 16> kTalkUpdateExpected = {
  0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b,
  0xd9, 0x0f, 0x29, 0x74, 0x24, 0x20, 0x48, 0x8b
};

// Totori's, and identical across both of its builds -- including the call
// displacement, because the callee sits at the same relative distance in each.
constexpr std::array<BYTE, 16> kTalkUpdateExpectedTotori = {
  0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x89, 0x20,
  0x01, 0x00, 0x00, 0xe8, 0x70, 0xf2, 0xff, 0xff
};

uintptr_t nodeOf(uintptr_t self) {
  uintptr_t node = 0;
  std::memcpy(&node, reinterpret_cast<const void*>(self + kNodeOffset),
              sizeof(node));
  return node;
}

void holdNodeAcrossUpdate(uintptr_t self, bool capture,
                          std::array<float, 32>& matrix, bool& have) {
  const uintptr_t node = nodeOf(self);
  if (!node)
    return;
  const uintptr_t at = node + kNodeMatrixOffset;
  if (capture) {
    have = readableRange(at, kNodeMatrixSize);
    if (have)
      std::memcpy(matrix.data(), reinterpret_cast<const void*>(at),
                  kNodeMatrixSize);
    return;
  }
  if (!have || !writableRange(at, kNodeMatrixSize))
    return;
  // Whether the update actually moved the node, not merely whether the hold
  // ran. It runs every frame of every conversation and writes the same bytes
  // back when there was no disagreement, so counting engagements says nothing
  // about whether anything was corrected. This counts corrections.
  const bool changed =
    std::memcmp(reinterpret_cast<const void*>(at), matrix.data(),
                kNodeMatrixSize) != 0;
  std::memcpy(reinterpret_cast<void*>(at), matrix.data(), kNodeMatrixSize);
  if (changed) {
    static std::atomic<uint32_t> corrected{0};
    const uint32_t n = corrected.fetch_add(1, std::memory_order_relaxed);
    if (n < 4 || (verboseLogging() && n % 4096 == 0))
      log("TALKANCHOR the update had moved the node; put back (n=", std::dec,
          n + 1, ")");
  }
}

void STDMETHODCALLTYPE tracedFieldUpdate(uintptr_t self, float dt) {
  // Before the update, so the resolver this call drives reads the value meant
  // for this frame.
  applyThreshold(dt);
  // Also before it, because the grace hold works by taking away the vertical
  // velocity the update is about to integrate.
  applyGraceHold(self);

  std::array<float, 32> nodeMatrix{};
  bool haveMatrix = false;
  const bool hold = talkAnchorEnabled() && conversationOnScreen();
  if (hold)
    holdNodeAcrossUpdate(self, true, nodeMatrix, haveMatrix);

  originalFieldUpdate(self, dt);

  if (hold && haveMatrix) {
    holdNodeAcrossUpdate(self, false, nodeMatrix, haveMatrix);
    // The controller's copy follows the node, because the update's last act was
    // to read one from the other and leaving them apart only moves the
    // disagreement somewhere else.
    if (writableRange(self + kPosOffset, sizeof(float) * 3))
      std::memcpy(reinterpret_cast<void*>(self + kPosOffset),
                  nodeMatrix.data() + 28, sizeof(float) * 3);
    static std::atomic<uint32_t> held{0};
    const uint32_t n = held.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || (verboseLogging() && n % 4096 == 0))
      log("TALKANCHOR node held at the anchor (n=", std::dec, n + 1, ")");
  }

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
}

}  // namespace

bool installFieldPhysics(BYTE* base, const Game& game) {
  const bool wantFix = engineFixEnabled();
  const bool wantGrace = graceHoldEnabled();
  const GroundRayAddrs* rayAddrs =
    groundRayEnabled() ? groundRayAddressesFor(game) : nullptr;
  if (!wantFix && !wantGrace && !rayAddrs) {
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
  if (!matches(base + addrs->update, updateExpected)) {
    log("FIELDPHYS declined: unexpected controller-update prologue");
    return false;
  }
  // The threshold is only meaningful if the function reading it is the one we
  // think it is.
  const std::array<BYTE, 16> resolverExpected = {
    0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0xe8,
  };
  if (wantFix && !matches(base + addrs->collisionResolver, resolverExpected)) {
    log("FIELDPHYS declined: unexpected collision-resolver prologue");
    return false;
  }
  if (wantFix && !prepareThreshold(base, *addrs))
    return false;
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
    g_graceActive = false;
    g_groundRayActive = false;
  }
  // Independent of the physics fixes above: the talk hook only reports whether
  // a conversation is on screen, and the anchor hold is its only reader. It
  // declines on its own terms rather than taking them down with it.
  if (installed && talkAnchorEnabled()) {
    if (!addrs->talkUpdate) {
      log("FIXES talk_anchor=no_address for this build");
    } else {
      BYTE* talk = base + addrs->talkUpdate;
      const bool totori = game.atlasVariant == AtlasTotori;
      if (!(totori ? matches(talk, kTalkUpdateExpectedTotori)
                   : matches(talk, kTalkUpdateExpected))) {
        log("TALKANCHOR talk-state prologue mismatch at 0x", std::hex,
            addrs->talkUpdate, std::dec, "; the hold stays off");
      } else if (!installMinHookDetour(talk,
                   reinterpret_cast<void*>(&tracedTalkUpdate),
                   reinterpret_cast<void**>(&originalTalkUpdate))) {
        log("TALKANCHOR talk-state hook failed; the hold stays off");
      } else {
        log("FIXES talk_anchor=active, gated on the talk state at 0x",
            std::hex, addrs->talkUpdate, std::dec);
      }
    }
  }

  log("FIXES field_physics=", installed ? "active" : "failed",
      " engine_fix=", g_moveThreshold ? 1 : 0,
      " grace_hold=", g_graceActive ? 1 : 0,
      " ground_ray=", g_groundRayActive ? 1 : 0);
  return installed;
}

}  // namespace atfix
