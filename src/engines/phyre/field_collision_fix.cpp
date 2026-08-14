// SPDX-License-Identifier: MIT
//
// Field character collision corrections.
//
// Two independent defects, one shared cause of confusion. Both were found by
// instrumenting the field encounter path; the diagnostic that found them is
// a probe that has since been removed; it is in the repository history.
//
// ---------------------------------------------------------------------------
// 1. Monsters snap across the ground when their AI re-targets
// ---------------------------------------------------------------------------
//
// A field monster chasing the player runs a three-step cycle in its brain: mode
// 0 re-targets and builds a mover aimed at the player, mode 2 copies the
// mover's position into the brain's intended position every frame, and mode 1
// notices the mover has expired and starts the next segment. The mover's
// lifetime is set in its constructor as distance divided by speed, so with the
// monster held at a fixed separation from a stationary player the cycle is
// metronomic. Measured at about 540 ms.
//
// The intended position is turned into movement by FieldMapCharaBase::Update,
// which computes velocity = (intended - current) / dt, and the character
// controller then integrates position += velocity * dt. The frame delta
// cancels, so the whole gap is closed in a single frame however long that frame
// is. At the segment boundary the intended position moves abruptly twice, once
// when the mover snaps to its stale target and once when the next segment
// re-anchors, and the character is carried the entire distance in two frames.
//
// The displacement is therefore the same at any frame rate, which is what makes
// this easy to misread: it measures identical at 30, 60 and 200 fps. What
// changes with frame rate is how long those two frames last. The same 0.2 to
// 0.6 unit correction takes about 66 ms at 30 fps and about 10 ms at 200. At
// console frame rate it reads as a brisk step; at high refresh it reads as a
// teleport, and a monster that leaves the staff's reach between two frames
// takes the encounter with it.
//
// The correction is to spread the movement over time rather than to reduce it.
// The monster still reaches exactly the position the game asked for, and still
// on the same cycle; it simply cannot cover more ground per second than a
// walking character plausibly would. The limit is expressed as a speed, not as
// a distance per call, because a fixed per-call step would itself be frame-rate
// dependent and would reintroduce the problem in a different form.
//
// Only charas the field map lists as enemies are limited. The player, the
// party and every other character are untouched.
//
// ---------------------------------------------------------------------------
// 2. Characters are pulled together when they are too far apart
// ---------------------------------------------------------------------------
//
// The engine's character-versus-character separation routine computes the
// horizontal distance between two controllers and derives a push depth as the
// sum of their radii minus that distance. The result is signed and nothing
// clamps it, so when the pair is reported with the two further apart than their
// combined radius the depth is negative and the push reverses: the two are
// drawn together until the distance equals the radii sum exactly. That is an
// equality constraint where a separation constraint belongs, and it parks a
// monster on a fixed ring around the player rather than merely keeping it from
// overlapping. It was measured pulling a monster inward from 0.867 to 0.800.
//
// The fix is max(depth, 0), applied as a small code patch because the clamp
// does not fit in the available bytes without displacing instructions that
// cannot be put back. Genuine overlap still separates exactly as before.
//
// This one is not what produced the snapping above, and fixing it alone left
// that symptom unchanged. It is corrected because it is wrong, not because it
// was the cause.
//
// Unlike the rate limit, this patch is not scoped to enemies: it is a change to
// the shared routine, so it applies to every character pair including the
// player and party members. That is the wider of the two changes and has its
// own configuration key so it can be turned off independently.
//
// ---------------------------------------------------------------------------
//
// Both cover all six Arland executables and both ship on by default in all
// three games; the capability matrix in game.cpp is the source of truth.
//
// The rate limit needs per-build tick, setter and container addresses, so each
// build has its own row in kSnapBuilds below, read from its own disassembly
// rather than ported.
//
// The separation clamp is PhyreEngine's own defect and is present in every
// Arland build, and in Ayesha. The same instruction sequence with the same
// register allocation occurs exactly once per executable, so one signature
// covers all six and only the address differs.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "../../core/config.h"
#include "field_collision_fix.h"
#include "../../core/game.h"
#include "../../core/log.h"
#include "../../core/mem.h"

namespace atfix {

extern Log log;  // main.cpp

namespace {

// Totori English RVAs.
//
// 0x8ba30  the field map's per-frame subsystem tick, (fieldMap, dt). Supplies
//          both the frame delta and, through [fieldMap+0xa0], the chara manager
//          that owns the enemy vector.
// 0x41bdf0 the engine's world-position setter, (object, const float* xyzw). It
//          writes [node+0x140] and back-computes the local transform. Every
//          field character move reaches the scene through here.
// The unclamped depth subtraction inside the character separation routine
// (0x420ee0 in Totori English) is per-build; see kPullSites.
// Per-build wiring for the rate limit.
//
// Totori's per-frame entry is the field map's subsystem tick, whose argument is
// the field map with the chara manager hanging off it. Rorona and Meruru name
// the family properly in RTTI (nspFM::clsFM*) and expose a tighter entry, the
// chara manager's own update, whose argument IS the manager. One descriptor
// covers both shapes: managerFromArg is the offset to apply, or zero when the
// argument is already the manager.
//
// The container offsets differ between Totori and the other two, and so does
// chara-to-node: Totori uses +0xa8 where Rorona and Meruru use +0xa0. Getting
// that wrong would read an arbitrary pointer, so each was verified in the
// game's own disassembly rather than carried across.
//
// All six builds are covered. Totori's multilingual entry was derived
// independently rather than ported: the setter came from the same unique
// byte signature the other builds use, the tick from a homologue MATCH with a
// raw-identical prologue and equal size, and the three offsets were re-read in
// the multilingual disassembly rather than assumed from the English build.
struct SnapBuild {
  Title title;
  uint8_t build;
  uintptr_t tickRva;
  const std::array<BYTE, 16>* tickExpected;
  uintptr_t setterRva;
  const std::array<BYTE, 16>* setterExpected;
  uintptr_t managerFromArg;
  uintptr_t enemyBegin;
  uintptr_t enemyEnd;
  uintptr_t charaNode;
};

// The separation defect is PhyreEngine's, not Totori's: the same instruction
// sequence with the same register allocation appears once in every Arland
// build, and in Ayesha. Only the address differs, so one signature covers all
// six and this is a lookup rather than six code paths.
struct PullSite {
  Title title;
  uint8_t build;
  uintptr_t rva;
};
constexpr PullSite kPullSites[] = {
  { Title::Totori, BuildEnglish,      0x42118b },
  { Title::Totori, BuildMultilingual, 0x69e78b },
  { Title::Rorona, BuildEnglish,      0x54e53b },
  { Title::Rorona, BuildMultilingual, 0x56440b },
  { Title::Meruru, BuildEnglish,      0x5017fb },
  { Title::Meruru, BuildMultilingual, 0x500deb },
};

uintptr_t pullSiteFor(Title title, uint8_t build) {
  for (const PullSite& site : kPullSites)
    if (site.title == title && site.build == build)
      return site.rva;
  return 0;
}

// The node's resolved world translation, and the setter's node slot: both are
// engine layout and verified unchanged in every build.
constexpr uintptr_t kNodeTranslationOffset = 0x140;
// Selected at install time.
const SnapBuild* activeSnap = nullptr;
// The world-position setter's object holds its node here.
constexpr uintptr_t kSetterNodeOffset = 0x20;

// Totori: FieldMap subsystem tick. Byte-identical in both Totori builds.
constexpr std::array<BYTE, 16> kTickTotori = {
  0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
  0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0x0f,
};
// Rorona and Meruru: clsFMCharacterCore::Update. Byte-identical in all four of
// those builds, which is what a shared codebase looks like.
constexpr std::array<BYTE, 16> kTickCharaCore = {
  0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83,
  0xec, 0x30, 0x48, 0x8b, 0xf9, 0x0f, 0x29, 0x74,
};
// The setter's prologue carries the stack-cookie RIP displacement in bytes 7
// to 10, so it differs per build and each needs its own window.
constexpr std::array<BYTE, 16> kSetterTotoriEn = {
  0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x05, 0xd5,
  0x0b, 0x89, 0x00, 0x48, 0x33, 0xc4, 0x48, 0x89,
};
constexpr std::array<BYTE, 16> kSetterTotoriMulti = {
  0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x05, 0x85,
  0x2b, 0x97, 0x00, 0x48, 0x33, 0xc4, 0x48, 0x89,
};
constexpr std::array<BYTE, 16> kSetterRoronaEn = {
  0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x05, 0x35,
  0x5e, 0xb5, 0x00, 0x48, 0x33, 0xc4, 0x48, 0x89,
};
constexpr std::array<BYTE, 16> kSetterRoronaMulti = {
  0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x05, 0x65,
  0xd0, 0xb7, 0x00, 0x48, 0x33, 0xc4, 0x48, 0x89,
};
constexpr std::array<BYTE, 16> kSetterMeruruEn = {
  0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x05, 0x55,
  0xec, 0xa9, 0x00, 0x48, 0x33, 0xc4, 0x48, 0x89,
};
constexpr std::array<BYTE, 16> kSetterMeruruMulti = {
  0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x05, 0x35,
  0x52, 0xb0, 0x00, 0x48, 0x33, 0xc4, 0x48, 0x89,
};

constexpr SnapBuild kSnapBuilds[] = {
  // title          build              tick       tick window     setter     setter window        mgr    begin  end    node
  { Title::Totori, BuildEnglish,      0x8ba30,  &kTickTotori,     0x41bdf0, &kSetterTotoriEn,    0xa0,  0xd0,  0xd8,  0xa8 },
  { Title::Totori, BuildMultilingual, 0x2a8110, &kTickTotori,    0x6993f0, &kSetterTotoriMulti, 0xa0,  0xd0,  0xd8,  0xa8 },
  { Title::Rorona, BuildEnglish,      0x37ad60, &kTickCharaCore, 0x553130, &kSetterRoronaEn,    0,     0x38,  0x40,  0xa0 },
  { Title::Rorona, BuildMultilingual, 0x390330, &kTickCharaCore, 0x569000, &kSetterRoronaMulti, 0,     0x38,  0x40,  0xa0 },
  { Title::Meruru, BuildEnglish,      0x37c390, &kTickCharaCore, 0x5051e0, &kSetterMeruruEn,    0,     0x38,  0x40,  0xa0 },
  { Title::Meruru, BuildMultilingual, 0x379700, &kTickCharaCore, 0x5047d0, &kSetterMeruruMulti, 0,     0x38,  0x40,  0xa0 },
};

const SnapBuild* snapBuildFor(Title title, uint8_t build) {
  for (const SnapBuild& b : kSnapBuilds)
    if (b.title == title && b.build == build)
      return &b;
  return nullptr;
}
// subss xmm3, xmm9 followed by mulss xmm6, xmm8. Ten bytes, which is enough for
// a five-byte jump plus padding.
constexpr std::array<BYTE, 10> kDepthClampExpected = {
  0xf3, 0x41, 0x0f, 0x5c, 0xd9,
  0xf3, 0x41, 0x0f, 0x59, 0xf0,
};

using FieldTickProc = void (STDMETHODCALLTYPE*)(uintptr_t, float);
using SetWorldPositionProc = uintptr_t (STDMETHODCALLTYPE*)(uintptr_t, uintptr_t);

FieldTickProc originalFieldTick = nullptr;
SetWorldPositionProc originalSetWorldPosition = nullptr;

// How fast a limited monster may be carried, in world units per second. Six is
// comfortably above a walking character and far below a snap; with it active
// the largest correction the engine requested across a test session fell from
// 0.87 units to 0.086. Tunable for A/B work without a rebuild.
constexpr float kDefaultSnapSpeed = 6.0f;
constexpr float kMinimumStep = 0.02f;

float snapSpeed() {
  static const float speed = [] {
    const char* value = std::getenv("ARLAND_MONSTER_SNAP_SPEED");
    if (!value)
      return kDefaultSnapSpeed;
    const double parsed = std::atof(value);
    return parsed > 0.0 ? static_cast<float>(parsed) : kDefaultSnapSpeed;
  }();
  return speed;
}

struct Vec3 {
  float x, y, z;
};

// The field runs on one thread, so this needs no synchronisation.
// Rorona and Meruru reserve 128 entries, so a 64-slot bound would silently skip
// a busy map rather than fail loudly.
constexpr size_t kMaxEnemies = 192;
uintptr_t enemyNodes[kMaxEnemies] = {};
size_t enemyNodeCount = 0;
float lastFrameDelta = 1.0f / 60.0f;
bool limiterActive = false;

void snapshotEnemyNodes(uintptr_t tickArg) {
  enemyNodeCount = 0;
  if (!activeSnap)
    return;
  uintptr_t manager = tickArg, begin = 0, end = 0;
  if (activeSnap->managerFromArg &&
      (!tryRead(tickArg + activeSnap->managerFromArg, manager) || !manager))
    return;
  if (!tryRead(manager + activeSnap->enemyBegin, begin) ||
      !tryRead(manager + activeSnap->enemyEnd, end))
    return;
  if (!begin || end <= begin ||
      (end - begin) > kMaxEnemies * sizeof(uintptr_t))
    return;
  for (uintptr_t p = begin; p < end && enemyNodeCount < kMaxEnemies;
       p += sizeof(uintptr_t)) {
    uintptr_t chara = 0, node = 0;
    if (!tryRead(p, chara) || !chara)
      continue;
    if (!tryRead(chara + activeSnap->charaNode, node) || !node)
      continue;
    enemyNodes[enemyNodeCount++] = node;
  }
}

bool isEnemyNode(uintptr_t node) {
  for (size_t i = 0; i < enemyNodeCount; ++i)
    if (enemyNodes[i] == node)
      return true;
  return false;
}

float horizontalDistance(float ax, float az, float bx, float bz) {
  const float dx = ax - bx;
  const float dz = az - bz;
  const float sq = dx * dx + dz * dz;
  if (sq <= 0.0f)
    return 0.0f;
  // One Newton loop off a rough seed, to avoid pulling <cmath> into a function
  // that runs once per character per frame.
  float guess = sq > 1.0f ? sq * 0.5f : 1.0f;
  for (int i = 0; i < 12; ++i)
    guess = 0.5f * (guess + sq / guess);
  return guess;
}

void STDMETHODCALLTYPE limitedFieldTick(uintptr_t fieldMap, float dt) {
  if (dt > 0.0f)
    lastFrameDelta = dt;
  snapshotEnemyNodes(fieldMap);
  originalFieldTick(fieldMap, dt);
}

uintptr_t STDMETHODCALLTYPE limitedSetWorldPosition(uintptr_t object,
                                                    uintptr_t destination) {
  if (!limiterActive)
    return originalSetWorldPosition(object, destination);

  // Unguarded on purpose. This runs once per character per frame, and every
  // read below is of memory the original function itself is about to use: the
  // object owns the call, it null-checks the same node pointer at its own
  // entry and then writes through it, and the destination is the caller's own
  // buffer which the original reads four floats from. A guarded read here would
  // issue a page-query syscall per character per frame and prove nothing the
  // original does not already rely on. The enemy snapshot, which walks a vector
  // whose bounds the mod does not control, stays fully guarded.
  const uintptr_t node =
    *reinterpret_cast<const uintptr_t*>(object + kSetterNodeOffset);
  if (!node || !isEnemyNode(node))
    return originalSetWorldPosition(object, destination);

  const Vec3 current =
    *reinterpret_cast<const Vec3*>(node + kNodeTranslationOffset);
  const float* wanted = reinterpret_cast<const float*>(destination);

  float limit = snapSpeed() * lastFrameDelta;
  if (limit < kMinimumStep)
    limit = kMinimumStep;
  const float distance =
    horizontalDistance(wanted[0], wanted[2], current.x, current.z);
  if (distance <= limit || distance <= 0.0001f)
    return originalSetWorldPosition(object, destination);

  // Same direction, no further than the limit. The vertical lane is passed
  // through: height belongs to the separate ground snap, not to this.
  const float scale = limit / distance;
  float clamped[4] = {
    current.x + (wanted[0] - current.x) * scale,
    wanted[1],
    current.z + (wanted[2] - current.z) * scale,
    wanted[3],
  };
  return originalSetWorldPosition(
    object, reinterpret_cast<uintptr_t>(clamped));
}

// A jmp rel32 reaches 2GB, so the trampoline has to sit near the code it is
// spliced into. Walk outward from the target in allocation-granularity steps.
BYTE* allocateNear(uintptr_t target, size_t size) {
  SYSTEM_INFO info = {};
  GetSystemInfo(&info);
  const uintptr_t step = info.dwAllocationGranularity
    ? info.dwAllocationGranularity : 0x10000;
  for (uintptr_t delta = step; delta < 0x40000000; delta += step) {
    for (int direction = 0; direction < 2; ++direction) {
      const uintptr_t candidate = direction ? target + delta : target - delta;
      if (candidate < step)
        continue;
      void* got = VirtualAlloc(
        reinterpret_cast<void*>(candidate & ~(step - 1)), size,
        MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
      if (got)
        return static_cast<BYTE*>(got);
    }
  }
  return nullptr;
}

// Replace `subss xmm3, xmm9` and the `mulss` after it with a jump to a
// trampoline that performs the subtraction, clamps the result against zero and
// replays the displaced instruction.
bool installDepthClamp(BYTE* base, uintptr_t rva) {
  BYTE* site = base + rva;
  if (!matches(site, kDepthClampExpected)) {
    log("FIXES field_character_pull=signature_mismatch");
    return false;
  }
  BYTE* tramp = allocateNear(reinterpret_cast<uintptr_t>(site), 64);
  if (!tramp) {
    log("FIXES field_character_pull=no_trampoline_in_range");
    return false;
  }

  size_t n = 0;
  const BYTE subss[] = {0xf3, 0x41, 0x0f, 0x5c, 0xd9};
  std::memcpy(tramp + n, subss, sizeof(subss)); n += sizeof(subss);
  const BYTE maxss[] = {0xf3, 0x0f, 0x5f, 0x1d};   // maxss xmm3, [rip+zero]
  std::memcpy(tramp + n, maxss, sizeof(maxss)); n += sizeof(maxss);
  const int32_t zeroDisp = 10;   // the zero sits ten bytes past this instruction
  std::memcpy(tramp + n, &zeroDisp, 4); n += 4;
  const BYTE mulss[] = {0xf3, 0x41, 0x0f, 0x59, 0xf0};
  std::memcpy(tramp + n, mulss, sizeof(mulss)); n += sizeof(mulss);
  tramp[n++] = 0xe9;
  const int32_t back = static_cast<int32_t>(
    (site + kDepthClampExpected.size()) - (tramp + n + 4));
  std::memcpy(tramp + n, &back, 4); n += 4;
  const float zero = 0.0f;
  std::memcpy(tramp + n, &zero, 4); n += 4;
  FlushInstructionCache(GetCurrentProcess(), tramp, n);

  DWORD previous = 0;
  if (!VirtualProtect(site, kDepthClampExpected.size(),
                      PAGE_EXECUTE_READWRITE, &previous)) {
    VirtualFree(tramp, 0, MEM_RELEASE);
    log("FIXES field_character_pull=protect_failed");
    return false;
  }
  site[0] = 0xe9;
  const int32_t toTramp = static_cast<int32_t>(tramp - (site + 5));
  std::memcpy(site + 1, &toTramp, 4);
  std::memset(site + 5, 0x90, kDepthClampExpected.size() - 5);
  DWORD ignored = 0;
  VirtualProtect(site, kDepthClampExpected.size(), previous, &ignored);
  FlushInstructionCache(GetCurrentProcess(), site, kDepthClampExpected.size());
  return true;
}

}  // namespace

bool installFieldCollisionFix(BYTE* base, const Game& game) {
  const bool snapSupported =
    featureSupport(Feature::FieldMonsterSnap) != Support::Unsupported;
  const bool pullSupported =
    featureSupport(Feature::FieldCharacterPull) != Support::Unsupported;
  if (!snapSupported && !pullSupported) {
    log("FIXES field_collision=not_applicable");
    return false;
  }
  const Title title = currentTitle();
  const SnapBuild* snap = snapBuildFor(title, game.exeBuild);

  bool snapInstalled = false;
  if (snap && featureEnabled(Feature::FieldMonsterSnap)) {
    BYTE* tick = base + snap->tickRva;
    BYTE* setter = base + snap->setterRva;
    if (matches(tick, *snap->tickExpected) &&
        matches(setter, *snap->setterExpected)) {
      activeSnap = snap;
      // The enemy snapshot must be in place before anything can be limited, so
      // the tick goes in first; on a partial install the limiter stays inert.
      if (installMinHookDetour(tick, reinterpret_cast<void*>(&limitedFieldTick),
                               reinterpret_cast<void**>(&originalFieldTick)) &&
          installMinHookDetour(
            setter, reinterpret_cast<void*>(&limitedSetWorldPosition),
            reinterpret_cast<void**>(&originalSetWorldPosition))) {
        limiterActive = true;
        snapInstalled = true;
      } else {
        activeSnap = nullptr;
      }
    }
  }

  bool pullInstalled = false;
  const uintptr_t pullRva = pullSiteFor(title, game.exeBuild);
  if (pullRva && featureEnabled(Feature::FieldCharacterPull))
    pullInstalled = installDepthClamp(base, pullRva);

  log("FIXES field_collision monster_snap=",
      !snap ? "not_applicable"
        : !featureEnabled(Feature::FieldMonsterSnap) ? "off"
        : snapInstalled ? "active" : "failed",
      " character_pull=",
      !pullRva ? "not_applicable"
        : !featureEnabled(Feature::FieldCharacterPull) ? "off"
        : pullInstalled ? "active" : "failed",
      " snap_speed=", snapSpeed());
  return snapInstalled || pullInstalled;
}

}  // namespace atfix
