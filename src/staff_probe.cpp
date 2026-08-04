// SPDX-License-Identifier: MIT
//
// Field staff-swing probe (Totori English, diagnostic only).
//
// Play reports that at high refresh rates a staff swing often fails to start a
// battle while walking into a monster stays reliable. Static analysis found no
// frame-rate coupling anywhere on the detection path, and an instrumented run
// at 60 Hz and 200 fps confirmed that: the hit window measures the same wall
// clock at both, and the detection sampling scales with refresh, so high
// refresh gives more chances to land a hit rather than fewer.
//
// What the same run did surface is what happens when a swing connects and the
// monster declines the encounter. Detection runs every frame and forks on
// whether the player is mid-swing: the staff range test runs first and the
// plain body-contact test runs when it reports nothing. The staff test is a
// pure distance check, a probe point 0.8 units in front of the character
// against each enemy's own radius plus 1.2, with no per-frame quantity in it.
//
// The paths differ in what happens after a hit. When the hit came from the
// staff, and only then, the game asks the enemy's brain whether it evades, and
// that call rolls the shared rand() against a percentage on the enemy's data
// record. A won roll starts no encounter and queues brain mode 7.
//
// Modes 7 to 11 are not a flee behaviour. They are a fixed sequence: write a
// 1 -> 0 tween over 0.5 seconds, wait for it, relocate the monster outright to
// a position stored in its own data record, write a 0 -> 1 tween over 0.5
// seconds, wait for it, return to normal. That is the shape of a fade out,
// teleport, fade in, and it is why a monster appears to blink to somewhere
// else rather than run away.
//
// The code that ticks that tween and applies it has not been located
// statically, because the mode handlers address the struct through a hoisted
// base and the offset scanner cannot see accesses of that form. So two
// different defects would look identical in play: a tween whose timer is
// advanced per frame rather than by elapsed time, which at high refresh would
// finish the fade far too early and expose the teleport, or a tween that is
// timed correctly but never reaches the material, which would leave the
// monster fully visible until it blinks. The first is frame-rate dependent and
// the second is not, and they need different fixes.
//
// This probe separates them by sampling the tween itself rather than timing it
// from outside.
//
// Hooks, none of which changes behaviour unless ARLAND_STAFF_FORCE_EVADE is
// set:
//
//   1. the staff range test, to count frames and hits. Its calls also delimit
//      swings: the engine only calls it while the swing's hit window is open,
//      so a gap in the call stream is the gap between two swings and no hook
//      on the swing trigger itself is needed.
//   2. the enemy evade roll, to report the threshold and the outcome, and
//      optionally to force the roll (see below).
//   3. the game's rand(), armed only for the duration of one evade roll, to
//      report the value the roll drew. Optional: without it the report loses
//      the drawn value and keeps everything else.
//   4. the brain mode dispatcher, to sample the tween fields on every frame a
//      tween is live. This is what shows whether the timer drains at the rate
//      elapsed time implies and whether the opacity actually ramps.
//   5. the relocation itself, to timestamp the teleport against the evade that
//      caused it. The sequence spends 0.5 seconds in mode 8 first, so that
//      interval must read about 500 ms at every refresh rate.
//
// ARLAND_STAFF_FORCE_EVADE=1 makes every staff hit evade, so the sequence can
// be reproduced on demand instead of waiting for a monster whose percentage is
// above zero. It raises the record's threshold above the highest possible roll
// for the duration of the original call and writes the original value straight
// back, so nothing is left modified. This one does change behaviour and is
// deliberately a separate switch from the probe.
//
// Addresses are Totori English only. The brain family is renamed in Rorona and
// Meruru with different vtable widths, and homologue matching returns WEAK for
// the evade roll against both, so nothing here ports by address.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "game.h"
#include "log.h"
#include "mem.h"
#include "staff_probe.h"
#include "util.h"

namespace atfix {

extern Log log;  // main.cpp

namespace {

// Depth clamp: the actual defect.
//
// 0x420ee0 is PhyreEngine's character-vs-character depenetration. It computes
// the horizontal distance d between two controllers, normalises the offset, and
// derives a push depth:
//
//   0x421172  movss xmm3, [rdi+0xb4]     ; radius A
//   0x42117e  addss xmm3, [rbx+0xb4]     ; radius A + radius B
//   0x421186  divss xmm8, xmm9           ; 1/d, so the normal is normalised
//   0x42118b  subss xmm3, xmm9           ; depth = (rA + rB) - d
//   0x4211a6  mulss xmm1, xmm13          ; depth * weight, used directly
//
// depth is signed and nothing clamps it. When the pair is reported with the
// bodies further apart than rA + rB the depth is negative, the push reverses,
// and the two are pulled TOGETHER until the distance equals rA + rB exactly.
// That is an equality constraint rather than a separation constraint, and it is
// what parks a monster permanently on a 0.8 unit ring around the player: the
// measured sample 0.866779 -> 0.799999 is this instruction pulling inward and
// stopping precisely at the radii sum.
//
// The fix is max(depth, 0). The site needs a trampoline rather than an in-place
// rewrite: the clamp does not fit in the ten bytes available without displacing
// instructions that will not fit back.
//
// The guard above it, comiss xmm9, xmm15 / jbe, is not a substitute. xmm15 is
// loaded at 0x4210a1 from 0x8431f8, which reads 0x37480000, about 1.14e-5. It
// only rejects a near-zero distance and sends that case to a separate fallback.
//
// The fallback's own subtraction at 0x42128c is NOT the same defect and must be
// left alone: it is reached only when d is already below that epsilon, so its
// xmm9 is effectively zero and its depth is the full radii sum, which is the
// correct response to coincident bodies.
constexpr uintptr_t kDepthClampRva = 0x42118b;
constexpr std::array<BYTE, 10> kDepthClampExpected = {
  0xf3, 0x41, 0x0f, 0x5c, 0xd9,  // subss xmm3, xmm9
  0xf3, 0x41, 0x0f, 0x59, 0xf0,  // mulss xmm6, xmm8
};

bool depthClampEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_STAFF_DEPTH_CLAMP");
    return value && value[0] != '0';
  }();
  return enabled;
}

// A jmp rel32 reaches +-2GB, so the trampoline has to live near the code it is
// spliced into. Walk outward from the target in allocation-granularity steps.
BYTE* allocateNear(uintptr_t target, size_t size) {
  SYSTEM_INFO info = {};
  GetSystemInfo(&info);
  const uintptr_t step = info.dwAllocationGranularity
    ? info.dwAllocationGranularity : 0x10000;
  const uintptr_t limit = 0x40000000;  // 1GB either way, well inside rel32
  for (uintptr_t delta = step; delta < limit; delta += step) {
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

bool installDepthClamp(BYTE* base) {
  BYTE* site = base + kDepthClampRva;
  if (!matches(site, kDepthClampExpected)) {
    log("STAFFPROBE depth_clamp=signature_mismatch at 0x", std::hex,
        kDepthClampRva, std::dec, "; not patching");
    return false;
  }

  BYTE* tramp = allocateNear(reinterpret_cast<uintptr_t>(site), 64);
  if (!tramp) {
    log("STAFFPROBE depth_clamp=no_trampoline_within_range");
    return false;
  }

  // subss xmm3, xmm9 | maxss xmm3, [rip+zero] | mulss xmm6, xmm8 | jmp back
  size_t n = 0;
  const BYTE subss[] = {0xf3, 0x41, 0x0f, 0x5c, 0xd9};
  std::memcpy(tramp + n, subss, sizeof(subss)); n += sizeof(subss);

  const BYTE maxss[] = {0xf3, 0x0f, 0x5f, 0x1d};
  std::memcpy(tramp + n, maxss, sizeof(maxss)); n += sizeof(maxss);
  const int32_t zeroDisp = 10;  // zero sits 10 bytes past the end of this insn
  std::memcpy(tramp + n, &zeroDisp, 4); n += 4;

  const BYTE mulss[] = {0xf3, 0x41, 0x0f, 0x59, 0xf0};
  std::memcpy(tramp + n, mulss, sizeof(mulss)); n += sizeof(mulss);

  tramp[n++] = 0xe9;
  const int32_t backDisp = static_cast<int32_t>(
    (site + kDepthClampExpected.size()) - (tramp + n + 4));
  std::memcpy(tramp + n, &backDisp, 4); n += 4;

  const float zero = 0.0f;
  std::memcpy(tramp + n, &zero, 4); n += 4;
  FlushInstructionCache(GetCurrentProcess(), tramp, n);

  DWORD previous = 0;
  if (!VirtualProtect(site, kDepthClampExpected.size(), PAGE_EXECUTE_READWRITE,
                      &previous)) {
    log("STAFFPROBE depth_clamp=protect_failed");
    return false;
  }
  site[0] = 0xe9;
  const int32_t toTramp = static_cast<int32_t>(tramp - (site + 5));
  std::memcpy(site + 1, &toTramp, 4);
  std::memset(site + 5, 0x90, kDepthClampExpected.size() - 5);
  DWORD ignored = 0;
  VirtualProtect(site, kDepthClampExpected.size(), previous, &ignored);
  FlushInstructionCache(GetCurrentProcess(), site, kDepthClampExpected.size());

  log("STAFFPROBE depth_clamp=active site=0x", std::hex, kDepthClampRva,
      " trampoline=", reinterpret_cast<uintptr_t>(tramp), std::dec,
      " (max(depth,0) applied to character depenetration)");
  return true;
}

bool probeEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_STAFF_PROBE");
    return value && value[0] != '0';
  }();
  return enabled;
}

bool forceEvadeEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_STAFF_FORCE_EVADE");
    return value && value[0] != '0';
  }();
  return enabled;
}

// Observation mode: swings and contact still run detection, and the probe
// still reports hits, but nothing comes of them. The evade roll is answered
// "did not evade" without running, so no monster dodges, and the encounter
// trigger is dropped, so no battle starts. That leaves a field where a monster
// can be hit repeatedly while its ordinary movement is watched.
bool noEncounterEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_STAFF_NO_ENCOUNTER");
    return value && value[0] != '0';
  }();
  return enabled;
}

// Totori English RVAs.
//
// 0x91eb0  the staff range test, called from the detection fork at 0x91d60.
//          Arguments are (chara, outRecord, outEnemyId, outSpawnId); the out
//          record is a byte "hit" at +0 and the winning distance at +4.
// 0xa9280  FieldMapCharaBaseBrainEnemy vtable slot 22, reached only on the
//          staff path. Returns true to mean "evaded, start no encounter".
// 0x5a7f58 the game's rand(): seed = seed * 0x343FD + 0x269EC3, returns
//          (seed >> 16) & 0x7FFF. Shared by 215 call sites.
// 0x9bbc0  the brain mode dispatcher, (brain, dt). A 13-entry jump table at
//          0x9bef4 indexed by mode + 1. Runs once per brain per frame.
// 0x9eae0  mode 9's action, (brain, dt): builds the destination from the
//          monster's data record and relocates it through 0xa84f0.
constexpr uintptr_t kStaffRangeTestRva = 0x91eb0;
constexpr uintptr_t kEvadeRollRva = 0xa9280;
constexpr uintptr_t kGameRandRva = 0x5a7f58;
constexpr uintptr_t kBrainModeUpdateRva = 0x9bbc0;
constexpr uintptr_t kFleeRelocateRva = 0x9eae0;
// 0xa84f0  the chara position setter, (chara, const float* xyzw). Eighteen
//          call sites reach it and the flee relocation is only one of them, so
//          hooking the setter and reporting the return address identifies
//          which mechanism moved a monster without having to guess first.
constexpr uintptr_t kSetPositionRva = 0xa84f0;
// 0x66580  the encounter trigger, (request, enemyId, spawnId, advantage),
//          called from 0x91d60 once a hit has survived the evade check. Its
//          return value is ignored by the caller, so dropping the call is a
//          clean way to suppress battles without touching detection.
// 0x91d60  the detection fork, (fieldMap). Runs once per frame and picks
//          between the staff range test and the body-contact test. Hooked only
//          to sample the player position from [fieldMap+0xa0], which is the
//          same field the staff test reads.
// 0xa8620  the local-transform writer, (object, vector, flag). Writes the
//          node's local 4x4 directly, so moves through it never reach the
//          position setter at 0xa84f0. Four static callers.
constexpr uintptr_t kLocalMoveRva = 0xa8620;
constexpr uintptr_t kDetectionForkRva = 0x91d60;
constexpr uintptr_t kEncounterTriggerRva = 0x66580;
// The trigger has two callers and only one of them is walk-into-it detection.
// The other, 0x956d0, fires a queued encounter from FieldMapStateNormal::Update
// when a pending spawn id has been left on the field map, which is the scripted
// or forced battle route. Suppressing that as well could wedge a story event,
// so observation mode drops the call only when it came from the detection fork.
// This is the return address inside 0x91d60, not the call site.
constexpr uintptr_t kDetectionTriggerReturn = 0x91e7a;

// The enemy data record holding the evade percentage, and the field inside it.
// The roll is rand() % 101 and evades when the draw is below this value, so a
// threshold of 101 evades on every possible draw.
constexpr uintptr_t kBrainDataRecordOffset = 0x1a8;
constexpr uintptr_t kEvadePercentOffset = 0x4c;
constexpr int kRollModulus = 101;
constexpr int32_t kAlwaysEvade = 101;

// The chara the brain drives, and the tween the mode sequence writes on it.
// Mode 7 writes (1.0, 0.0, 1.0, 0.5) and mode 10 writes (0.0, 1.0, 0.0, 0.5),
// which reads as from, to, a flag, and a duration. The chara constructors rest
// "from" and "to" at 1.0, consistent with an opacity that sits fully visible.
constexpr uintptr_t kBrainCharaOffset = 0x120;
constexpr uintptr_t kTweenFromOffset = 0x158;
constexpr uintptr_t kTweenToOffset = 0x15c;
constexpr uintptr_t kTweenFlagOffset = 0x160;
constexpr uintptr_t kTweenTimerOffset = 0x164;

constexpr std::array<BYTE, 16> kStaffRangeTestExpected = {
  0x40, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48,
  0x81, 0xec, 0x98, 0x00, 0x00, 0x00, 0x48, 0x8b,
};
constexpr std::array<BYTE, 16> kEvadeRollExpected = {
  0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
  0x01, 0x48, 0x8b, 0xd9, 0xff, 0x90, 0x98, 0x00,
};
// These two windows cover a relative call and a RIP displacement, so they are
// build-specific. That is acceptable because the whole probe is gated to
// Totori English anyway.
constexpr std::array<BYTE, 16> kGameRandExpected = {
  0x48, 0x83, 0xec, 0x28, 0xe8, 0x7b, 0xae, 0x04,
  0x00, 0x69, 0x48, 0x28, 0xfd, 0x43, 0x03, 0x00,
};
constexpr std::array<BYTE, 16> kFleeRelocateExpected = {
  0x4c, 0x8b, 0xdc, 0x53, 0x48, 0x81, 0xec, 0xa0,
  0x00, 0x00, 0x00, 0x48, 0x8b, 0x05, 0xde, 0xde,
};
constexpr std::array<BYTE, 16> kBrainModeUpdateExpected = {
  0x40, 0x55, 0x53, 0x41, 0x56, 0x48, 0x8b, 0xec,
  0x48, 0x83, 0xec, 0x50, 0x48, 0x8b, 0x01, 0x48,
};
constexpr std::array<BYTE, 16> kSetPositionExpected = {
  0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74,
  0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48,
};
constexpr std::array<BYTE, 16> kLocalMoveExpected = {
  0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x57,
  0x48, 0x81, 0xec, 0xf0, 0x00, 0x00, 0x00, 0x0f,
};
constexpr std::array<BYTE, 16> kDetectionForkExpected = {
  0x40, 0x53, 0x55, 0x48, 0x83, 0xec, 0x28, 0x48,
  0x8b, 0x41, 0x68, 0x48, 0x8b, 0xd9, 0x48, 0x8b,
};
constexpr std::array<BYTE, 16> kEncounterTriggerExpected = {
  0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41,
  0x57, 0x48, 0x8d, 0x6c, 0x24, 0x98, 0x48, 0x81,
};

// Where the resolved world transform keeps its translation row, so the hook
// can compare where a chara is against where it is being put.
constexpr uintptr_t kCharaTransformOffset = 0xa8;
constexpr uintptr_t kTransformTranslationOffset = 0x140;

// Squared distance beyond which a position write is a jump rather than a step.
// The staff itself only reaches 0.8 + 1.2 units, so 2 units is comfortably
// above anything ordinary movement produces in one frame.
constexpr float kTeleportThresholdSq = 4.0f;

struct Vec3 {
  float x, y, z;
};

using StaffRangeTestProc =
  void (STDMETHODCALLTYPE*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using EvadeRollProc = BYTE (STDMETHODCALLTYPE*)(uintptr_t);
using GameRandProc = int (STDMETHODCALLTYPE*)();
using BrainModeUpdateProc = BYTE (STDMETHODCALLTYPE*)(uintptr_t, float);
using FleeRelocateProc = void (STDMETHODCALLTYPE*)(uintptr_t, float);
using SetPositionProc = uintptr_t (STDMETHODCALLTYPE*)(uintptr_t, uintptr_t);
using EncounterTriggerProc =
  uintptr_t (STDMETHODCALLTYPE*)(uintptr_t, uint32_t, uint32_t, uint32_t);
using DetectionForkProc = BYTE (STDMETHODCALLTYPE*)(uintptr_t);
using LocalMoveProc =
  uintptr_t (STDMETHODCALLTYPE*)(uintptr_t, uintptr_t, uintptr_t);

StaffRangeTestProc originalStaffRangeTest = nullptr;
EvadeRollProc originalEvadeRoll = nullptr;
GameRandProc originalGameRand = nullptr;
BrainModeUpdateProc originalBrainModeUpdate = nullptr;
FleeRelocateProc originalFleeRelocate = nullptr;
SetPositionProc originalSetPosition = nullptr;
EncounterTriggerProc originalEncounterTrigger = nullptr;
DetectionForkProc originalDetectionFork = nullptr;
LocalMoveProc originalLocalMove = nullptr;

// Resolved at install time so a return address can be reported as an RVA,
// which is what the static tables and this file's comments speak.
uintptr_t moduleBase = 0;

uint64_t qpcFrequency() {
  static const uint64_t frequency = [] {
    LARGE_INTEGER value = {};
    QueryPerformanceFrequency(&value);
    return value.QuadPart ? static_cast<uint64_t>(value.QuadPart) : 1u;
  }();
  return frequency;
}

uint64_t qpcNow() {
  LARGE_INTEGER value = {};
  QueryPerformanceCounter(&value);
  return static_cast<uint64_t>(value.QuadPart);
}

int elapsedMs(uint64_t from, uint64_t to) {
  if (to <= from)
    return 0;
  return static_cast<int>(((to - from) * 1000ull) / qpcFrequency());
}

// True if [p, p+n) is committed and writable. mem.h only proves readability,
// and the forced-evade path writes into a game data record.
bool writableRange(uintptr_t p, size_t n) {
  if (!p)
    return false;
  MEMORY_BASIC_INFORMATION mbi = {};
  if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)))
    return false;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD))
    return false;
  const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
    PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  if (!(mbi.Protect & writable))
    return false;
  const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  return p >= base && p + n <= base + mbi.RegionSize;
}

// Write an int32 even when the page is read-only. The monster data records
// live in a read-only mapping, which is why the first forced-evade attempt
// silently did nothing: a plain writability test rejects them. Protection is
// restored immediately, and the caller writes the original value straight back
// afterwards, so nothing is left changed.
bool writeInt32Unprotected(uintptr_t addr, int32_t value) {
  if (writableRange(addr, sizeof(int32_t))) {
    *reinterpret_cast<int32_t*>(addr) = value;
    return true;
  }
  DWORD previous = 0;
  if (!VirtualProtect(reinterpret_cast<void*>(addr), sizeof(int32_t),
                      PAGE_READWRITE, &previous))
    return false;
  *reinterpret_cast<int32_t*>(addr) = value;
  DWORD ignored = 0;
  VirtualProtect(reinterpret_cast<void*>(addr), sizeof(int32_t), previous,
                 &ignored);
  return true;
}

// Address of the evade threshold on this brain's data record, or 0.
uintptr_t evadePercentAddress(uintptr_t brain) {
  uintptr_t record = 0;
  if (!tryRead(brain + kBrainDataRecordOffset, record) || !record)
    return 0;
  const uintptr_t field = record + kEvadePercentOffset;
  return readableRange(field, sizeof(int32_t)) ? field : 0;
}

// A swing is a run of consecutive frames in which the staff test was called.
// The engine calls it only while the hit window is open, so any pause longer
// than a few frames means the previous swing ended. 250 ms is far longer than
// one frame at any refresh rate this mod supports and far shorter than the gap
// between two deliberate swings. Measured windows are about 213 ms, 43 frames
// at 200 fps and 14 at 60 Hz, so this threshold clears both comfortably.
constexpr int kSwingGapMs = 250;

struct SwingRecord {
  uint64_t startTicks;
  uint64_t lastTicks;
  uint32_t frames;     // frames the staff test ran
  uint32_t hitFrames;  // frames it reported an enemy in staff range
  uint32_t rolls;      // evade rolls the swing caused
  uint32_t evaded;     // rolls the enemy won
  bool active;
};

// The field detection path and the brain updates run on the game's logic thread
// only, so these need no synchronisation. The rand capture below is the
// exception: rand() is shared with other threads, so its capture slot is
// thread-local.
SwingRecord swing = {};
uint32_t swingIndex = 0;

// When the most recent evade was won, so the relocation can be timed against
// it, and a cap so a stuck tween cannot fill the log.
uint64_t lastEvadeTicks = 0;
bool evadePending = false;
uint32_t tweenSamples = 0;
constexpr uint32_t kMaxTweenSamples = 4000;
constexpr int kSequenceWindowMs = 3000;

thread_local bool insideEvadeRoll = false;
thread_local int capturedRand = -1;

// Per-frame position watch.
//
// Hooking the position setter proved that nothing moves a chara more than two
// units through that function during play, yet monsters visibly jump. So the
// jump either bypasses that setter, by writing the transform directly, or the
// visible model is decoupled from the logical position. Sampling each brain's
// chara position once per frame and reporting frame-to-frame discontinuities
// catches the first case whatever the mechanism, and its silence would be
// strong evidence for the second.
//
// A monster walking at a few units per second covers hundredths of a unit in
// one frame at any refresh rate this runs at, so half a unit in a single frame
// is far above ordinary movement and far below anything that would read as a
// teleport on screen.
struct PositionSlot {
  uintptr_t chara;
  uintptr_t transform;    // validated, so ordinary frames skip the guards
  Vec3 last;              // position after the previous frame's brain update
  Vec3 preUpdate;         // position before this frame's brain update
  uint32_t validFrames;   // frames left before the chain is re-proved
  uint64_t lastSeen;      // for eviction; see below
  bool seeded;
};

// Slots must be reclaimable. A battle destroys and recreates every chara, so
// on return to the field the pointers are all new. Without eviction the table
// stays full of the pre-battle set, every new chara fails to find a slot, and
// tracking silently stops for the rest of the session. That is exactly how a
// post-battle relocation went unrecorded.
uint64_t positionTick = 0;
constexpr size_t kPositionSlots = 48;
// 0.15 units in one frame. A monster walking at a few units per second covers
// hundredths of a unit per frame at any refresh rate here, so this still sits
// an order of magnitude above ordinary movement. Lowered from 0.5 because the
// relocation seen in normal play, as opposed to the sustained-contact case,
// may be smaller than anything measured so far.
constexpr float kFrameJumpThresholdSq = 0.0225f;
constexpr uint32_t kMaxJumpReports = 300;
// How long a proved pointer chain is trusted. The guarded walk costs a
// VirtualQuery per level, which is the expensive part of sampling every frame
// under Proton, and the chain only changes when the object is destroyed.
constexpr uint32_t kRevalidateFrames = 64;
PositionSlot positionCache[kPositionSlots] = {};
uint32_t jumpReports = 0;

// Totori's position, sampled once per frame from the detection fork.
//
// The first attempt picked her out of the brain dispatcher by matching the
// FieldMapCharaBaseBrainPad vtable, on the assumption that her brain runs
// through the same dispatcher as every monster's. It never matched, and the
// run came back with the player at the origin on every line. Rather than guess
// why, this takes the route that is known to run: the detection fork executes
// once per frame and holds the field map, which holds the player chara at a
// fixed offset that the staff test itself reads.
constexpr uintptr_t kFieldMapPlayerCharaOffset = 0xa0;
Vec3 playerPosition = {};
bool playerPositionKnown = false;

// Totori's collision proxy and its two radii.
//
// The swing handler calls 0x9d3d0 on the player chara, which reaches this
// object. That function carries a branch setting the active radius to the base
// radius times 25, which would be an enormous collider, but its only caller
// passes zero and the state byte it guards on is only ever written zero, so on
// static reading the branch never runs. Sampling both radii at the moment a
// monster jumps settles whether that reading is right, and if the active
// radius is ever inflated it also gives the exact figure to compare the
// post-jump separation against.
constexpr uintptr_t kCharaCollisionOffset = 0xc0;
constexpr uintptr_t kColliderActiveRadiusOffset = 0xac;
constexpr uintptr_t kColliderBaseRadiusOffset = 0xe8;
constexpr uintptr_t kColliderSwingFlagOffset = 0x38;
// The field map's enemy vector, [fieldMap+0xd0] to [fieldMap+0xd8], holding
// chara pointers. The detection tests walk exactly this range, so membership in
// it is the game's own definition of "is an enemy".
//
// This exists because the object being reported as jumping holds a distance of
// exactly 0.800 to the player, to within 1e-4, permanently. A monster walking
// after the player would not hold a distance to four decimal places; something
// positioned rigidly relative to the player would, and 0.8 is the staff probe's
// own forward offset. Before any more work goes into finding what moves it, it
// has to be established that it is a monster at all.
constexpr uintptr_t kFieldMapEnemyBeginOffset = 0xd0;
constexpr uintptr_t kFieldMapEnemyEndOffset = 0xd8;
constexpr size_t kMaxEnemies = 64;
uintptr_t enemyCharas[kMaxEnemies] = {};
size_t enemyCharaCount = 0;

void snapshotEnemies(uintptr_t fieldMap) {
  uintptr_t begin = 0, end = 0;
  enemyCharaCount = 0;
  if (!tryRead(fieldMap + kFieldMapEnemyBeginOffset, begin) ||
      !tryRead(fieldMap + kFieldMapEnemyEndOffset, end))
    return;
  if (!begin || end <= begin || (end - begin) > kMaxEnemies * sizeof(uintptr_t))
    return;
  for (uintptr_t p = begin; p < end; p += sizeof(uintptr_t)) {
    uintptr_t chara = 0;
    if (!tryRead(p, chara) || !chara)
      continue;
    if (enemyCharaCount < kMaxEnemies)
      enemyCharas[enemyCharaCount++] = chara;
  }
}

bool isEnemyChara(uintptr_t chara) {
  for (size_t i = 0; i < enemyCharaCount; ++i)
    if (enemyCharas[i] == chara)
      return true;
  return false;
}

float playerColliderActive = -1.0f;
float playerColliderBase = -1.0f;
int playerColliderFlag = -1;

// Recent calls to the local-transform writer, 0xa8620.
//
// That function writes a node's whole local 4x4 (translation at +0xd0, then
// three more rows), which is how a move can bypass the position setter the
// earlier hook watched. Its first argument is not a chara: it stores sixteen
// bytes at +0xa0, which would overwrite the +0xa8 node pointer a chara keeps
// there. Rather than guess the type, this records the raw pointer and the
// caller for the last few calls, and the jump report prints them. Whichever
// caller consistently precedes a jump is the one to read.
struct MoveCall {
  uintptr_t object;
  uintptr_t callerRva;
  Vec3 position;  // [object+0xa0] at the time of the call
};
constexpr size_t kMoveRing = 8;
MoveCall moveRing[kMoveRing] = {};
size_t moveRingNext = 0;

// Who last set a given node's local transform.
//
// The write watch named the writing instruction, 0x45503 inside 0x45450, but
// that function is generic engine plumbing with eleven callers: it sanitises a
// matrix and stores it, and says nothing about who wanted the move. Recording
// the caller per node closes the last hop, because the jump report already
// holds the node address of the monster that moved.
constexpr uintptr_t kSetLocalTransformRva = 0x45450;
constexpr std::array<BYTE, 16> kSetLocalTransformExpected = {
  0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x81,
  0xec, 0xa0, 0x00, 0x00, 0x00, 0x41, 0x0f, 0x28,
};
using SetLocalTransformProc =
  uintptr_t (STDMETHODCALLTYPE*)(uintptr_t, uintptr_t, uintptr_t);
SetLocalTransformProc originalSetLocalTransform = nullptr;

struct NodeSetter {
  uintptr_t node;
  uintptr_t callerRva;
  uintptr_t previousCallerRva;
};
constexpr size_t kNodeSetters = 32;
NodeSetter nodeSetters[kNodeSetters] = {};

void recordNodeSetter(uintptr_t node, uintptr_t callerRva) {
  size_t free = kNodeSetters;
  for (size_t i = 0; i < kNodeSetters; ++i) {
    if (nodeSetters[i].node == node) {
      if (nodeSetters[i].callerRva != callerRva)
        nodeSetters[i].previousCallerRva = nodeSetters[i].callerRva;
      nodeSetters[i].callerRva = callerRva;
      return;
    }
    if (!nodeSetters[i].node && free == kNodeSetters)
      free = i;
  }
  if (free < kNodeSetters) {
    nodeSetters[free].node = node;
    nodeSetters[free].callerRva = callerRva;
    nodeSetters[free].previousCallerRva = 0;
  }
}

void reportNodeSetter(uintptr_t node) {
  for (size_t i = 0; i < kNodeSetters; ++i) {
    if (nodeSetters[i].node != node)
      continue;
    log("STAFFPROBE   lastTransformSetter callerRva=0x", std::hex,
        nodeSetters[i].callerRva, " previous=0x",
        nodeSetters[i].previousCallerRva, std::dec);
    return;
  }
  log("STAFFPROBE   lastTransformSetter none recorded for this node");
}

// Stack capture at the world-position setter.
//
// 0x41bdf0 assembles a vec4 from four floats, writes it straight into the
// node's world translation at +0x140, and back-computes the local transform.
// It is an explicit teleport primitive, but it is engine plumbing: three
// callers, each of which is itself generic. Walking up one level per run would
// take several more runs and keep landing on middleware, so this records the
// whole chain at once for the enemies already being tracked, and the jump
// report prints the chain for the monster that moved.
constexpr uintptr_t kSetWorldPositionRva = 0x41bdf0;
constexpr std::array<BYTE, 16> kSetWorldPositionExpected = {
  0x48, 0x83, 0xec, 0x48, 0x48, 0x8b, 0x05, 0xd5,
  0x0b, 0x89, 0x00, 0x48, 0x33, 0xc4, 0x48, 0x89,
};
using SetWorldPositionProc = uintptr_t (STDMETHODCALLTYPE*)(uintptr_t, uintptr_t);
SetWorldPositionProc originalSetWorldPosition = nullptr;

// Resolved from ntdll rather than linked, so a missing import cannot break the
// build on either toolchain.
using CaptureStackFn = USHORT (WINAPI*)(ULONG, ULONG, PVOID*, PULONG);
CaptureStackFn captureStackBackTrace = nullptr;

constexpr size_t kStackDepth = 12;
constexpr size_t kStackSlots = 8;
struct StackRecord {
  uintptr_t node;
  uintptr_t frames[kStackDepth];
  USHORT count;
};
StackRecord stackRecords[kStackSlots] = {};

void recordStack(uintptr_t node) {
  if (!captureStackBackTrace)
    return;
  size_t index = kStackSlots;
  for (size_t i = 0; i < kStackSlots; ++i) {
    if (stackRecords[i].node == node) { index = i; break; }
    if (!stackRecords[i].node && index == kStackSlots) index = i;
  }
  if (index == kStackSlots)
    index = 0;  // overwrite the first rather than lose the newest
  PVOID frames[kStackDepth] = {};
  const USHORT got = captureStackBackTrace(1, kStackDepth, frames, nullptr);
  stackRecords[index].node = node;
  stackRecords[index].count = got;
  for (USHORT i = 0; i < got; ++i)
    stackRecords[index].frames[i] = reinterpret_cast<uintptr_t>(frames[i]);
}

void reportStack(uintptr_t node) {
  for (size_t i = 0; i < kStackSlots; ++i) {
    if (stackRecords[i].node != node)
      continue;
    for (USHORT f = 0; f < stackRecords[i].count; ++f) {
      const uintptr_t raw = stackRecords[i].frames[f];
      log("STAFFPROBE     stack[", f, "] rva=0x", std::hex,
          moduleBase && raw > moduleBase ? raw - moduleBase : raw, std::dec);
    }
    return;
  }
}

// True if this node belongs to an enemy the position watch is tracking.
bool isTrackedNode(uintptr_t node);
float horizontalDistance(const Vec3& a, const Vec3& b);

// True while the staff's hit window is open. The engine calls the staff range
// test only during that window, so a recent call is the window being open. The
// tolerance covers the physics step running either side of detection within the
// same frame, which the ordering has not been established for.
constexpr int kSwingWindowGraceMs = 50;
bool swingWindowOpen() {
  return swing.active &&
         elapsedMs(swing.lastTicks, qpcNow()) <= kSwingWindowGraceMs;
}

// Experiment: drop the physics reposition for enemies while a swing is live.
//
// This is the falsifiable test of the whole causal claim. If suppressing only
// this one call makes staff hits register reliably, the reposition is what
// breaks them. It is deliberately surgical rather than skipping the physics
// tick outright, which would freeze collision for everything on the map.
//
// It does change behaviour, and it discards a result the solver has already
// computed, so the physics state and the transform disagree for a frame. That
// is acceptable for an experiment behind its own switch and is not a shipping
// fix.
// 0 off, 1 only while the swing window is open, 2 always, 3 clamp large steps.
//
// The clamp limit: measured jumps are 0.5 to 1.3 units in one step while
// ordinary movement covers hundredths of a unit per frame, so this sits well
// clear of both.
// A speed, not a distance. A fixed per-call step is itself frame-rate
// dependent: 0.2 units per call is 40 units per second at 200 fps and 6 at 30,
// and the first measured run showed monsters darting because of exactly that.
// Walking pace is a few units per second, so this sits above normal movement
// and far below a snap. Tunable with ARLAND_STAFF_CLAMP_SPEED.
constexpr float kClampSpeedDefault = 6.0f;
constexpr float kClampFloor = 0.02f;

float clampSpeed() {
  static const float speed = [] {
    const char* value = std::getenv("ARLAND_STAFF_CLAMP_SPEED");
    if (!value) return kClampSpeedDefault;
    const double parsed = std::atof(value);
    return parsed > 0.0 ? static_cast<float>(parsed) : kClampSpeedDefault;
  }();
  return speed;
}

// The most recent frame delta, taken from the brain update, so the clamp can be
// expressed in units per second.
float lastFrameDt = 1.0f / 60.0f;
//
// Mode 1 was the first attempt and it measured cleanly wrong: it fired several
// thousand times, so the interception point is right, yet every jump still
// landed with the nearest freeze 130 ms or more away. The repositions that
// matter happen in the gaps between swings, not during them, which agrees with
// the report that the monster moves just before a fight rather than mid-swing.
// Mode 2 removes the scoping so the jump can be eliminated outright and the
// encounter test can then be run against a monster that stays put.
//
// Mode 2 is a blunt instrument: enemies reach their positions through this
// path, so freezing it unconditionally will also stop them moving. That is
// acceptable for a test and is not a shipping fix.
int freezeEnemyMode() {
  static const int mode = [] {
    const char* value = std::getenv("ARLAND_STAFF_FREEZE_ENEMY");
    if (!value || value[0] == '0')
      return 0;
    if (value[0] == '2') return 2;
    if (value[0] == '3') return 3;
    return 1;
  }();
  return mode;
}

uint32_t freezeCount = 0;

uintptr_t STDMETHODCALLTYPE probedSetWorldPosition(uintptr_t object,
                                                    uintptr_t floats) {
  uintptr_t node = 0;
  if (tryRead(object + 0x20, node) && node && isTrackedNode(node)) {
    recordStack(node);
    const int mode = freezeEnemyMode();

    // Mode 3: let ordinary movement through and clamp only the discontinuity.
    //
    // Mode 2 proved the interception point by freezing enemies outright, but
    // that also stops them walking, so a gameplay test against a stationary
    // monster would prove nothing. This reads the position the solver wants,
    // compares it with where the node currently is, and when the step is larger
    // than anything ordinary movement produces, substitutes a position the same
    // direction but no further than the limit. The monster still reaches the
    // solver's target, over a few frames instead of instantly.
    //
    // This is the first thing here shaped like a real fix rather than a probe.
    if (mode == 3) {
      Vec3 current = {};
      float wanted[4] = {};
      if (tryRead(node + kTransformTranslationOffset, current) &&
          tryRead(floats, wanted)) {
        const float dx = wanted[0] - current.x;
        const float dz = wanted[2] - current.z;
        const float distance = horizontalDistance(
          Vec3{wanted[0], 0.0f, wanted[2]}, Vec3{current.x, 0.0f, current.z});
        float limit = clampSpeed() * lastFrameDt;
        if (limit < kClampFloor)
          limit = kClampFloor;
        if (distance > limit && distance > 0.0001f) {
          const float scale = limit / distance;
          static thread_local float clamped[4];
          clamped[0] = current.x + dx * scale;
          clamped[1] = wanted[1];  // vertical is the ground snap's business
          clamped[2] = current.z + dz * scale;
          clamped[3] = wanted[3];
          if (++freezeCount <= 20 || (freezeCount % 500) == 0)
            log("STAFFPROBE clamp n=", freezeCount, " node=", node,
                " wanted=", distance, " limitedTo=", limit,
                " dt=", lastFrameDt);
          return originalSetWorldPosition(
            object, reinterpret_cast<uintptr_t>(clamped));
        }
      }
    }

    if (mode == 2 || (mode == 1 && swingWindowOpen())) {
      if (++freezeCount <= 20 || (freezeCount % 200) == 0)
        log("STAFFPROBE freeze n=", freezeCount, " node=", node,
            " (reposition dropped during swing window)");
      return 0;
    }
  }
  return originalSetWorldPosition(object, floats);
}

uintptr_t STDMETHODCALLTYPE probedSetLocalTransform(uintptr_t node,
                                                    uintptr_t a,
                                                    uintptr_t b) {
  const uintptr_t raw =
    reinterpret_cast<uintptr_t>(arlandReturnAddress());
  recordNodeSetter(node,
                   moduleBase && raw > moduleBase ? raw - moduleBase : raw);
  return originalSetLocalTransform(node, a, b);
}

// Write watch on the position field.
//
// Everything else in this probe observes; this one traps. The publish path is
// mapped and innocent, and neither writer of [obj+0xa0] in the chara and AI
// region can produce a horizontal jump, so the writer is either in engine code
// or invisible to the offset scanner because it addresses the field through a
// hoisted base. Naming it needs the write itself caught.
//
// The page holding the field is made read-only, so writes fault and reads do
// not. A guard page would trip on every read as well, which on a page shared
// with other objects is thousands of exceptions a second. On a fault inside the
// watched field the faulting instruction is recorded, the page is restored, the
// trap flag is set so the write completes, and the single-step that follows
// re-arms the page.
//
// The handler records into a fixed array and never logs, because logging takes
// a lock and touches a file, and doing either inside an exception handler on an
// arbitrary thread is asking for a deadlock. The records are flushed from the
// ordinary per-frame path instead.
//
// Bounded on purpose: it disarms itself after a fixed number of faults or once
// it has collected enough distinct writers, so a mistake costs a short burst
// rather than the session.
constexpr size_t kMaxWatchRecords = 16;
constexpr uint32_t kMaxWatchFaults = 200000;
constexpr size_t kWritersWanted = 4;
constexpr size_t kWatchPageSize = 0x1000;

struct WatchRecord {
  uintptr_t writerRva;
  uintptr_t address;
  uint32_t count;
  bool flushed;
};

std::atomic<bool> watchArmed{false};
uintptr_t watchPageBase = 0;
uintptr_t watchFieldLo = 0;
uintptr_t watchFieldHi = 0;
uintptr_t watchObject = 0;
DWORD watchOriginalProtect = 0;
std::atomic<uint32_t> watchFaults{0};
WatchRecord watchRecords[kMaxWatchRecords] = {};
std::atomic<size_t> watchRecordCount{0};
thread_local bool watchSingleStepPending = false;
PVOID watchHandlerHandle = nullptr;

bool writeWatchEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_STAFF_WRITE_WATCH");
    return value && value[0] != '0';
  }();
  return enabled;
}

bool protectWatchPage(DWORD protection, DWORD* previous) {
  DWORD ignored = 0;
  return VirtualProtect(reinterpret_cast<void*>(watchPageBase), kWatchPageSize,
                        protection, previous ? previous : &ignored) != 0;
}

void disarmWriteWatch() {
  if (!watchArmed.exchange(false))
    return;
  protectWatchPage(watchOriginalProtect, nullptr);
}

LONG CALLBACK writeWatchHandler(PEXCEPTION_POINTERS info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;

  // The step that follows a trapped write: re-arm and carry on.
  if (code == EXCEPTION_SINGLE_STEP && watchSingleStepPending) {
    watchSingleStepPending = false;
    if (watchArmed.load(std::memory_order_relaxed))
      protectWatchPage(PAGE_READONLY, nullptr);
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  if (code != EXCEPTION_ACCESS_VIOLATION ||
      !watchArmed.load(std::memory_order_relaxed) ||
      info->ExceptionRecord->NumberParameters < 2 ||
      info->ExceptionRecord->ExceptionInformation[0] != 1)  // 1 == write
    return EXCEPTION_CONTINUE_SEARCH;

  const uintptr_t address = info->ExceptionRecord->ExceptionInformation[1];
  if (address < watchPageBase || address >= watchPageBase + kWatchPageSize)
    return EXCEPTION_CONTINUE_SEARCH;

  // Ours. Record only writes that land in the field, but let every write on the
  // page through, otherwise the page stays read-only under an unrelated store.
  if (address >= watchFieldLo && address < watchFieldHi) {
    const uintptr_t raw =
      reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    const uintptr_t rva =
      moduleBase && raw > moduleBase ? raw - moduleBase : raw;
    const size_t used = watchRecordCount.load(std::memory_order_relaxed);
    bool seen = false;
    for (size_t i = 0; i < used; ++i) {
      if (watchRecords[i].writerRva == rva) {
        ++watchRecords[i].count;
        seen = true;
        break;
      }
    }
    if (!seen && used < kMaxWatchRecords) {
      watchRecords[used].writerRva = rva;
      watchRecords[used].address = address;
      watchRecords[used].count = 1;
      watchRecords[used].flushed = false;
      watchRecordCount.store(used + 1, std::memory_order_relaxed);
    }
  }

  // The page also carries the resolved transform rows the resolver rewrites
  // every frame, so faults are frequent and the cap has to be generous. What
  // actually ends the watch is having collected enough distinct writers.
  if (watchRecordCount.load(std::memory_order_relaxed) >= kWritersWanted ||
      watchFaults.fetch_add(1, std::memory_order_relaxed) >= kMaxWatchFaults) {
    disarmWriteWatch();
    return EXCEPTION_CONTINUE_EXECUTION;
  }

  protectWatchPage(PAGE_READWRITE, nullptr);
  watchSingleStepPending = true;
  info->ContextRecord->EFlags |= 0x100;  // trap flag, so the write completes
  return EXCEPTION_CONTINUE_EXECUTION;
}

// Armed on a node's LOCAL translation at +0xd0, taken straight from the
// jumping chara. The transform resolver only reads that field, writing the
// resolved rows at +0x110 through +0x140 instead, so a write landing here is a
// real move rather than the per-frame resolve. Nothing has to be matched: the
// position watch already holds the node address for the chara that jumped.
void armWriteWatch(uintptr_t node) {
  if (!writeWatchEnabled() || watchArmed.load(std::memory_order_relaxed))
    return;
  if (!node || watchObject)
    return;
  const uintptr_t field = node + 0xd0;
  if (!readableRange(field, 16))
    return;
  watchObject = node;
  watchFieldLo = field;
  watchFieldHi = field + 16;
  watchPageBase = field & ~static_cast<uintptr_t>(kWatchPageSize - 1);
  // The field must not straddle the page, or half the writes are missed.
  if (watchFieldHi > watchPageBase + kWatchPageSize) {
    watchObject = 0;
    return;
  }
  if (!protectWatchPage(PAGE_READONLY, &watchOriginalProtect)) {
    watchObject = 0;
    return;
  }
  watchArmed.store(true, std::memory_order_relaxed);
  log("STAFFPROBE writewatch armed node=", node,
      " localTranslation=0x", std::hex, field, " page=0x", watchPageBase,
      std::dec);
}

// Called from the ordinary per-frame path, never from the handler.
void flushWriteWatch() {
  const size_t used = watchRecordCount.load(std::memory_order_relaxed);
  for (size_t i = 0; i < used; ++i) {
    if (watchRecords[i].flushed)
      continue;
    watchRecords[i].flushed = true;
    log("STAFFPROBE writewatch WRITER callerRva=0x", std::hex,
        watchRecords[i].writerRva, " address=0x", watchRecords[i].address,
        std::dec, " (first sighting)");
  }
}

// Prove chara and its transform are mapped, and cache the transform address.
bool revalidateSlot(PositionSlot& slot, uintptr_t chara) {
  uintptr_t transform = 0;
  Vec3 probe = {};
  if (!tryRead(chara + kCharaTransformOffset, transform) || !transform)
    return false;
  if (!tryRead(transform + kTransformTranslationOffset, probe))
    return false;
  slot.transform = transform;
  slot.validFrames = kRevalidateFrames;
  return true;
}

bool isTrackedNode(uintptr_t node) {
  for (size_t i = 0; i < kPositionSlots; ++i)
    if (positionCache[i].chara && positionCache[i].transform == node)
      return true;
  return false;
}

float horizontalDistance(const Vec3& a, const Vec3& b) {
  const float dx = a.x - b.x;
  const float dz = a.z - b.z;
  const float sq = dx * dx + dz * dz;
  if (sq <= 0.0f)
    return 0.0f;
  // One Newton step off a rough seed. Avoids pulling in <cmath> for a figure
  // that only has to be readable in a log.
  float guess = sq > 1.0f ? sq * 0.5f : 1.0f;
  for (int i = 0; i < 12; ++i)
    guess = 0.5f * (guess + sq / guess);
  return guess;
}

void reportJump(uintptr_t chara, uintptr_t node, const char* where,
                const Vec3& from, const Vec3& to, float dt) {
  const float dx = to.x - from.x;
  const float dy = to.y - from.y;
  const float dz = to.z - from.z;
  const float distanceSq = dx * dx + dy * dy + dz * dz;
  if (distanceSq <= kFrameJumpThresholdSq || jumpReports >= kMaxJumpReports)
    return;
  ++jumpReports;

  // Player-relative geometry, which is what decides whether this is Totori
  // pushing the monster out. A depenetration should show alignment close to
  // +1, meaning the jump points directly away from the player, and a
  // separation after the jump that lands at the same value every time,
  // which would be the sum of the two collision radii.
  float alignment = 0.0f;
  float separationBefore = -1.0f;
  float separationAfter = -1.0f;
  if (playerPositionKnown) {
    separationBefore = horizontalDistance(from, playerPosition);
    separationAfter = horizontalDistance(to, playerPosition);
    const float ox = from.x - playerPosition.x;
    const float oz = from.z - playerPosition.z;
    const float outLen = horizontalDistance(from, playerPosition);
    const float jumpLen = horizontalDistance(to, from);
    if (outLen > 0.0001f && jumpLen > 0.0001f)
      alignment = ((ox / outLen) * (dx / jumpLen)) + ((oz / outLen) * (dz / jumpLen));
  }

  log("STAFFPROBE jump n=", jumpReports,
      " where=", where,
      " chara=", chara,
      " isEnemy=", isEnemyChara(chara) ? "YES" : "no",
      " enemyCount=", enemyCharaCount,
      " dt=", dt,
      " from=(", from.x, ",", from.y, ",", from.z,
      ") to=(", to.x, ",", to.y, ",", to.z,
      ") distSq=", distanceSq,
      " player=(", playerPosition.x, ",", playerPosition.y, ",",
      playerPosition.z,
      ") sepBefore=", separationBefore,
      " sepAfter=", separationAfter,
      " alignAway=", alignment,
      " colliderActive=", playerColliderActive,
      " colliderBase=", playerColliderBase,
      " colliderFlag=", playerColliderFlag);

  reportNodeSetter(node);
  reportStack(node);
  // The jump that arms the watch is already over; the next one is caught.
  armWriteWatch(node);
}

uintptr_t STDMETHODCALLTYPE probedLocalMove(uintptr_t object, uintptr_t vector,
                                            uintptr_t flag) {
  const uintptr_t raw =
    reinterpret_cast<uintptr_t>(arlandReturnAddress());
  moveRing[moveRingNext].object = object;
  moveRing[moveRingNext].callerRva =
    moduleBase && raw > moduleBase ? raw - moduleBase : raw;
  // The published position, which is what lets a jumping monster be matched to
  // its object without knowing the object's type.
  Vec3 published = {};
  if (tryRead(object + 0xa0, published))
    moveRing[moveRingNext].position = published;
  moveRingNext = (moveRingNext + 1) % kMoveRing;
  return originalLocalMove(object, vector, flag);
}

// Sampled twice per frame, once either side of the brain update, so a jump is
// attributed rather than merely detected. "inside" means the brain's own mode
// handling moved the monster; "outside" means something between brain updates
// did, which would be movement, physics or animation root motion.
void watchCharaPosition(uintptr_t brain, bool afterUpdate, float dt) {
  // The brain is the object whose own method is running, so reading a field of
  // it needs no guard. Everything past this point leans on the slot's cached
  // validation instead of re-proving the chain every frame.
  const uintptr_t chara =
    *reinterpret_cast<const uintptr_t*>(brain + kBrainCharaOffset);
  if (!chara)
    return;

  // Track only charas the field map lists as enemies.
  //
  // Every chara in the scene passes through this dispatcher, and there are more
  // of them than the table holds. With eviction that means the whole table is
  // recycled every frame, no entry survives to be compared against the next
  // frame, and not one jump can ever be reported. Restricting to the enemy
  // vector bounds the tracked set to a handful and is what the investigation
  // actually needs.
  if (!isEnemyChara(chara))
    return;

  if (!afterUpdate)
    ++positionTick;

  // Find this chara's slot, a free slot, or the stalest slot to evict.
  size_t index = kPositionSlots;
  size_t fallback = 0;
  uint64_t oldest = ~uint64_t{0};
  for (size_t i = 0; i < kPositionSlots; ++i) {
    if (positionCache[i].chara == chara) {
      index = i;
      break;
    }
    if (!positionCache[i].chara) {
      if (oldest != 0) {
        oldest = 0;
        fallback = i;
      }
    } else if (positionCache[i].lastSeen < oldest) {
      oldest = positionCache[i].lastSeen;
      fallback = i;
    }
  }
  if (index == kPositionSlots) {
    if (afterUpdate)
      return;  // claim on the pre-update pass only
    index = fallback;
    positionCache[index] = PositionSlot{};
  }

  {
    PositionSlot& slot = positionCache[index];
    slot.lastSeen = positionTick;

    if (slot.chara != chara) {
      if (afterUpdate)
        return;  // claim a slot on the pre-update pass only
      slot = PositionSlot{};
      slot.chara = chara;
      slot.lastSeen = positionTick;  // or it is the next eviction candidate
      if (!revalidateSlot(slot, chara)) {
        slot.chara = 0;  // leave the slot free and try again next frame
        return;
      }
      slot.last = *reinterpret_cast<const Vec3*>(
        slot.transform + kTransformTranslationOffset);
      slot.preUpdate = slot.last;
      return;  // seeded on the matching post pass below
    }

    if (!afterUpdate && slot.validFrames == 0 && !revalidateSlot(slot, chara)) {
      slot = PositionSlot{};
      return;
    }
    if (!afterUpdate && slot.validFrames)
      --slot.validFrames;

    const Vec3 now = *reinterpret_cast<const Vec3*>(
      slot.transform + kTransformTranslationOffset);
    if (!afterUpdate) {
      if (slot.seeded)
        reportJump(chara, slot.transform, "outside", slot.last, now, dt);
      slot.preUpdate = now;
    } else {
      if (slot.seeded)
        reportJump(chara, slot.transform, "inside", slot.preUpdate, now, dt);
      slot.last = now;
      if (!slot.seeded) {
        static bool announced = false;
        if (!announced) {
          announced = true;
          log("STAFFPROBE tracking enemy chara=", chara,
              " enemyCount=", enemyCharaCount,
              " (position watch live; silence from here means no jumps)");
        }
      }
      slot.seeded = true;
    }
    return;
  }
}

void emitSwingSummary() {
  if (!swing.active)
    return;
  log("STAFFPROBE swing=", swingIndex,
      " frames=", swing.frames,
      " hitFrames=", swing.hitFrames,
      " rolls=", swing.rolls,
      " evaded=", swing.evaded,
      " windowMs=", elapsedMs(swing.startTicks, swing.lastTicks));
  swing.active = false;
}

void STDMETHODCALLTYPE probedStaffRangeTest(uintptr_t self, uintptr_t outRecord,
                                            uintptr_t outEnemyId,
                                            uintptr_t outSpawnId) {
  originalStaffRangeTest(self, outRecord, outEnemyId, outSpawnId);

  const uint64_t now = qpcNow();
  if (swing.active && elapsedMs(swing.lastTicks, now) > kSwingGapMs)
    emitSwingSummary();
  if (!swing.active) {
    swing = SwingRecord{};
    swing.active = true;
    swing.startTicks = now;
    ++swingIndex;
  }
  swing.lastTicks = now;
  ++swing.frames;

  BYTE hit = 0;
  if (tryRead(outRecord, hit) && hit)
    ++swing.hitFrames;
}

BYTE STDMETHODCALLTYPE probedEvadeRoll(uintptr_t self) {
  // Observation mode answers "did not evade" without running the original, so
  // nothing is queued into the dodge sequence. The battle this would otherwise
  // lead to is dropped at the trigger instead.
  if (noEncounterEnabled()) {
    if (swing.active)
      ++swing.rolls;
    log("STAFFPROBE roll swing=", swingIndex,
        " result=suppressed(no-encounter mode)");
    return 0;
  }

  capturedRand = -1;
  insideEvadeRoll = true;

  // Forced evade: lift the threshold above every possible draw for exactly the
  // duration of the original call, then put the record back as it was.
  const uintptr_t percentField =
    forceEvadeEnabled() ? evadePercentAddress(self) : 0;
  int32_t savedPercent = 0;
  const bool forcing = percentField &&
    tryRead(percentField, savedPercent) &&
    writeInt32Unprotected(percentField, kAlwaysEvade);
  if (forceEvadeEnabled() && !forcing) {
    static bool reported = false;
    if (!reported) {
      reported = true;
      log("STAFFPROBE force-evade UNAVAILABLE: record=0x", std::hex,
          percentField, std::dec, "; rolls proceed unforced");
    }
  }

  const BYTE evaded = originalEvadeRoll(self);

  if (forcing)
    writeInt32Unprotected(percentField, savedPercent);
  insideEvadeRoll = false;

  // The roll is skipped entirely when the enemy is already alerted or already
  // fleeing, in which case rand() was never reached and the draw stays -1.
  const int drawn = capturedRand;

  int32_t percent = -1;
  if (percentField)
    tryRead(percentField, percent);
  else {
    uintptr_t record = 0;
    if (tryRead(self + kBrainDataRecordOffset, record) && record)
      tryRead(record + kEvadePercentOffset, percent);
  }

  if (swing.active) {
    ++swing.rolls;
    if (evaded)
      ++swing.evaded;
  }

  if (evaded) {
    lastEvadeTicks = qpcNow();
    evadePending = true;
    tweenSamples = 0;
  }

  log("STAFFPROBE roll swing=", swingIndex,
      " rand=", drawn,
      " roll=", drawn >= 0 ? drawn % kRollModulus : -1,
      " evadePercent=", percent,
      forcing ? " (forced)" : "",
      " result=", evaded ? "evaded(no encounter)" : "connected(first strike)");
  return evaded;
}

int STDMETHODCALLTYPE probedGameRand() {
  const int value = originalGameRand();
  // Only the first draw inside one evade roll is the roll itself.
  if (insideEvadeRoll && capturedRand < 0)
    capturedRand = value;
  return value;
}

// Sample the tween on every frame it is live. This is the measurement that
// separates a fade running too fast from a fade that never reaches the screen:
// the timer says how the duration is being spent, and "from" says whether the
// opacity is actually moving.
BYTE STDMETHODCALLTYPE probedBrainModeUpdate(uintptr_t self, float dt) {
  // Sampled either side of the update so a jump can be attributed to the brain
  // or to whatever runs between brain updates. Runs unconditionally: the
  // position watch does not depend on knowing which mechanism moves a monster.
  lastFrameDt = dt > 0.0f ? dt : lastFrameDt;
  watchCharaPosition(self, false, dt);
  const BYTE result = originalBrainModeUpdate(self, dt);
  watchCharaPosition(self, true, dt);

  if (!evadePending || tweenSamples >= kMaxTweenSamples)
    return result;

  // The whole sequence is two half-second tweens with a relocation between
  // them. Past a few seconds any tween still running belongs to something
  // else, and sampling it would put misleading sinceEvadeMs values in the log.
  if (elapsedMs(lastEvadeTicks, qpcNow()) > kSequenceWindowMs) {
    evadePending = false;
    return result;
  }

  uintptr_t chara = 0;
  float timer = 0.0f;
  if (!tryRead(self + kBrainCharaOffset, chara) || !chara)
    return result;
  if (!tryRead(chara + kTweenTimerOffset, timer) || !(timer > 0.0f))
    return result;

  float from = 0.0f, to = 0.0f, flag = 0.0f;
  tryRead(chara + kTweenFromOffset, from);
  tryRead(chara + kTweenToOffset, to);
  tryRead(chara + kTweenFlagOffset, flag);

  ++tweenSamples;
  log("STAFFPROBE tween n=", tweenSamples,
      " sinceEvadeMs=", elapsedMs(lastEvadeTicks, qpcNow()),
      " dt=", dt,
      " timer=", timer,
      " from=", from,
      " to=", to,
      " flag=", flag);
  return result;
}

// Report position writes, two ways.
//
// Any jump beyond the threshold is reported outright, which is what catches a
// teleport. Separately, the first few calls from each distinct caller are
// reported whatever the distance, which builds a census of which of the
// eighteen sites actually run during play. The first run showed only two of
// them firing and both only at map load, so knowing which sites carry ordinary
// movement is what tells us whether a teleport would come through here at all.
uintptr_t STDMETHODCALLTYPE probedSetPosition(uintptr_t self,
                                              uintptr_t destination) {
  static uint32_t reported = 0;
  constexpr uint32_t kMaxReports = 200;

  // Small open-addressed census keyed by caller RVA. Fixed size, no allocation
  // and no locking, because this runs on the logic thread inside a hot call.
  constexpr size_t kCensusSlots = 32;
  constexpr uint32_t kSamplesPerCaller = 3;
  static uintptr_t censusCaller[kCensusSlots] = {};
  static uint32_t censusCount[kCensusSlots] = {};

  uintptr_t transform = 0;
  Vec3 before = {};
  Vec3 after = {};
  const bool haveBefore =
    tryRead(self + kCharaTransformOffset, transform) && transform &&
    tryRead(transform + kTransformTranslationOffset, before);

  if (haveBefore && reported < kMaxReports && tryRead(destination, after)) {
    const uintptr_t raw =
      reinterpret_cast<uintptr_t>(arlandReturnAddress());
    const uintptr_t caller =
      moduleBase && raw > moduleBase ? raw - moduleBase : raw;

    const float dx = after.x - before.x;
    const float dy = after.y - before.y;
    const float dz = after.z - before.z;
    const float distanceSq = dx * dx + dy * dy + dz * dz;

    bool censusWorthy = false;
    for (size_t i = 0; i < kCensusSlots; ++i) {
      if (!censusCaller[i])
        censusCaller[i] = caller;
      if (censusCaller[i] == caller) {
        if (censusCount[i] < kSamplesPerCaller) {
          ++censusCount[i];
          censusWorthy = true;
        }
        break;
      }
    }

    if (distanceSq > kTeleportThresholdSq || censusWorthy) {
      ++reported;
      log("STAFFPROBE ", distanceSq > kTeleportThresholdSq ? "teleport" : "move",
          " n=", reported,
          " callerRva=0x", std::hex, caller, std::dec,
          " chara=", self,
          " from=(", before.x, ",", before.y, ",", before.z,
          ") to=(", after.x, ",", after.y, ",", after.z,
          ") distSq=", distanceSq);
    }
  }
  return originalSetPosition(self, destination);
}

// Sample Totori once per frame. Guarded the whole way, because this runs once
// a frame rather than once per brain, so the cost of proving the chain is
// irrelevant here.
BYTE STDMETHODCALLTYPE probedDetectionFork(uintptr_t fieldMap) {
  // Emit anything the exception handler recorded. Done here, on the game
  // thread, because the handler must not take the log's lock.
  flushWriteWatch();
  snapshotEnemies(fieldMap);

  uintptr_t chara = 0;
  uintptr_t transform = 0;
  Vec3 position = {};
  if (tryRead(fieldMap + kFieldMapPlayerCharaOffset, chara) && chara &&
      tryRead(chara + kCharaTransformOffset, transform) && transform &&
      tryRead(transform + kTransformTranslationOffset, position)) {
    playerPosition = position;
    playerPositionKnown = true;

    uintptr_t collider = 0;
    if (tryRead(chara + kCharaCollisionOffset, collider) && collider) {
      float active = 0.0f, base = 0.0f;
      BYTE flag = 0;
      if (tryRead(collider + kColliderActiveRadiusOffset, active))
        playerColliderActive = active;
      if (tryRead(collider + kColliderBaseRadiusOffset, base))
        playerColliderBase = base;
      if (tryRead(collider + kColliderSwingFlagOffset, flag))
        playerColliderFlag = flag;
    }
  }
  return originalDetectionFork(fieldMap);
}

uintptr_t STDMETHODCALLTYPE probedEncounterTrigger(uintptr_t request,
                                                   uint32_t enemyId,
                                                   uint32_t spawnId,
                                                   uint32_t advantage) {
  const uintptr_t raw =
    reinterpret_cast<uintptr_t>(arlandReturnAddress());
  const uintptr_t caller =
    moduleBase && raw > moduleBase ? raw - moduleBase : raw;
  const bool fromDetection = caller == kDetectionTriggerReturn;

  if (noEncounterEnabled() && fromDetection) {
    log("STAFFPROBE encounter suppressed enemyId=", enemyId,
        " spawnId=", spawnId, " advantage=", advantage);
    return 0;
  }
  if (noEncounterEnabled()) {
    log("STAFFPROBE encounter allowed (not from detection) callerRva=0x",
        std::hex, caller, std::dec, " spawnId=", spawnId);
  }
  return originalEncounterTrigger(request, enemyId, spawnId, advantage);
}

void STDMETHODCALLTYPE probedFleeRelocate(uintptr_t self, float dt) {
  const int sinceEvade =
    evadePending ? elapsedMs(lastEvadeTicks, qpcNow()) : -1;
  log("STAFFPROBE warp sinceEvadeMs=", sinceEvade,
      " tweenSamples=", tweenSamples,
      " (mode 8 holds 0.5 s, so this should read about 500)");
  originalFleeRelocate(self, dt);
}

}  // namespace

bool installStaffProbe(BYTE* base, const Game& game) {
  if (!probeEnabled())
    return false;

  if (currentTitle() != Title::Totori || game.exeBuild != BuildEnglish) {
    log("FIXES staff_probe=not_applicable (Totori English only)");
    return false;
  }

  BYTE* staffTarget = base + kStaffRangeTestRva;
  BYTE* evadeTarget = base + kEvadeRollRva;
  BYTE* randTarget = base + kGameRandRva;
  BYTE* modeTarget = base + kBrainModeUpdateRva;
  BYTE* warpTarget = base + kFleeRelocateRva;
  BYTE* setPosTarget = base + kSetPositionRva;
  BYTE* triggerTarget = base + kEncounterTriggerRva;
  BYTE* forkTarget = base + kDetectionForkRva;
  BYTE* localMoveTarget = base + kLocalMoveRva;
  moduleBase = reinterpret_cast<uintptr_t>(base);

  if (!matches(staffTarget, kStaffRangeTestExpected) ||
      !matches(evadeTarget, kEvadeRollExpected)) {
    log("Staff-probe prologue mismatch staffTest=0x", std::hex,
        kStaffRangeTestRva, " evadeRoll=0x", kEvadeRollRva, std::dec,
        "; not installing");
    return false;
  }

  // The three annotating hooks go in first and their failures are not fatal:
  // each only records into slots the report reads, so a partial install loses
  // a column rather than the measurement.
  bool randInstalled = false;
  if (matches(randTarget, kGameRandExpected)) {
    randInstalled = installMinHookDetour(
      randTarget, reinterpret_cast<void*>(&probedGameRand),
      reinterpret_cast<void**>(&originalGameRand));
  }
  bool warpInstalled = false;
  if (matches(warpTarget, kFleeRelocateExpected)) {
    warpInstalled = installMinHookDetour(
      warpTarget, reinterpret_cast<void*>(&probedFleeRelocate),
      reinterpret_cast<void**>(&originalFleeRelocate));
  }
  bool tweenInstalled = false;
  if (matches(modeTarget, kBrainModeUpdateExpected)) {
    tweenInstalled = installMinHookDetour(
      modeTarget, reinterpret_cast<void*>(&probedBrainModeUpdate),
      reinterpret_cast<void**>(&originalBrainModeUpdate));
  }
  bool teleportInstalled = false;
  if (matches(setPosTarget, kSetPositionExpected)) {
    teleportInstalled = installMinHookDetour(
      setPosTarget, reinterpret_cast<void*>(&probedSetPosition),
      reinterpret_cast<void**>(&originalSetPosition));
  }
  // The write watch needs the local-move hook to identify which object
  // publishes a jumping monster, so it is only offered when that installs.
  // Registered first in the chain so it sees the fault before the crash
  // post-mortem does; it returns CONTINUE_SEARCH for anything not its own, so
  // the post-mortem still gets every real exception.
  if (writeWatchEnabled()) {
    watchHandlerHandle = AddVectoredExceptionHandler(1, &writeWatchHandler);
    log("FIXES staff_probe write_watch=",
        watchHandlerHandle ? "armed_on_first_jump" : "handler_install_failed");
  }

  if (!captureStackBackTrace) {
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
      captureStackBackTrace = reinterpret_cast<CaptureStackFn>(
        reinterpret_cast<void*>(
          GetProcAddress(ntdll, "RtlCaptureStackBackTrace")));
  }
  const bool depthClampInstalled =
    depthClampEnabled() ? installDepthClamp(base) : false;

  bool setWorldInstalled = false;
  {
    BYTE* target = base + kSetWorldPositionRva;
    if (matches(target, kSetWorldPositionExpected)) {
      setWorldInstalled = installMinHookDetour(
        target, reinterpret_cast<void*>(&probedSetWorldPosition),
        reinterpret_cast<void**>(&originalSetWorldPosition));
    }
  }

  bool setTransformInstalled = false;
  {
    BYTE* target = base + kSetLocalTransformRva;
    if (matches(target, kSetLocalTransformExpected)) {
      setTransformInstalled = installMinHookDetour(
        target, reinterpret_cast<void*>(&probedSetLocalTransform),
        reinterpret_cast<void**>(&originalSetLocalTransform));
    }
  }

  bool localMoveInstalled = false;
  if (matches(localMoveTarget, kLocalMoveExpected)) {
    localMoveInstalled = installMinHookDetour(
      localMoveTarget, reinterpret_cast<void*>(&probedLocalMove),
      reinterpret_cast<void**>(&originalLocalMove));
  }

  bool playerInstalled = false;
  if (matches(forkTarget, kDetectionForkExpected)) {
    playerInstalled = installMinHookDetour(
      forkTarget, reinterpret_cast<void*>(&probedDetectionFork),
      reinterpret_cast<void**>(&originalDetectionFork));
  }

  // The encounter suppressor is the one hook whose absence would silently
  // change what the run means, so a requested-but-failed install is loud.
  bool triggerInstalled = false;
  if (matches(triggerTarget, kEncounterTriggerExpected)) {
    triggerInstalled = installMinHookDetour(
      triggerTarget, reinterpret_cast<void*>(&probedEncounterTrigger),
      reinterpret_cast<void**>(&originalEncounterTrigger));
  }
  if (noEncounterEnabled() && !triggerInstalled)
    log("STAFFPROBE no-encounter UNAVAILABLE: trigger hook not installed;"
        " battles will still start");

  // The staff test next, so a failure on the evade roll still leaves swing and
  // hit counts rather than nothing at all.
  if (!installMinHookDetour(staffTarget,
                            reinterpret_cast<void*>(&probedStaffRangeTest),
                            reinterpret_cast<void**>(&originalStaffRangeTest))) {
    log("FIXES staff_probe=failed (staff range test)");
    return false;
  }
  if (!installMinHookDetour(evadeTarget,
                            reinterpret_cast<void*>(&probedEvadeRoll),
                            reinterpret_cast<void**>(&originalEvadeRoll))) {
    log("FIXES staff_probe=partial (evade roll not hooked)");
    return false;
  }

  log("FIXES staff_probe=active staffTest=0x", std::hex, kStaffRangeTestRva,
      " evadeRoll=0x", kEvadeRollRva, " rand=0x", kGameRandRva,
      " modeUpdate=0x", kBrainModeUpdateRva, " relocate=0x", kFleeRelocateRva,
      std::dec,
      " rollValues=", randInstalled ? "yes" : "no",
      " tween=", tweenInstalled ? "yes" : "no",
      " warp=", warpInstalled ? "yes" : "no",
      " teleport=", teleportInstalled ? "yes" : "no",
      " setTransform=", setTransformInstalled ? "yes" : "no",
      " setWorld=", setWorldInstalled ? "yes" : "no",
      " stacks=", captureStackBackTrace ? "yes" : "no",
      " freezeEnemy=", freezeEnemyMode(),
      " depthClamp=", depthClampInstalled ? "yes" : "no",
      " player=", playerInstalled ? "yes" : "no",
      " localMove=", localMoveInstalled ? "yes" : "no",
      " trigger=", triggerInstalled ? "yes" : "no",
      " forceEvade=", forceEvadeEnabled() ? "yes" : "no",
      " noEncounter=", noEncounterEnabled() ? "yes" : "no");
  return true;
}

}  // namespace atfix
