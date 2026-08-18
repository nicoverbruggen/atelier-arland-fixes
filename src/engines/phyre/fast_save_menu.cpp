// SPDX-License-Identifier: MIT
//
// Implementation. What this fixes and why it takes this shape is in
// fast_save_menu.h; what is here is the per-build wiring and the notes that
// only mean anything beside the code they sit on.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>

#include "../../core/game.h"
#include "../../core/hook_util.h"
#include "../../core/log.h"
#include "../../core/mem.h"
#include "fast_save_menu.h"
#include "../../core/page_patch.h"

namespace atfix {

extern Log log;  // main.cpp

namespace {

// Each gate is verified across a 16-byte window starting at the comiss, because
// that window carries the RIP displacement of the constant and so pins the site
// far better than the branch alone would. The branch sits at a known offset
// inside it. Windows are per build: the displacement differs even where the
// branch bytes are identical.
struct Gate {
  const char* name;
  uintptr_t comissRva;
  uint8_t branchOffset;   // from comissRva
  uint8_t branchLength;   // 2 for the short forms, 6 for the near form
  std::array<BYTE, 16> expected;
};

constexpr Gate kRoronaEn[] = {
  { "title_alloc", 0x21ef41, 7, 2, { 0x0f,0x2f,0x05,0x34,0x7d,0x4f,0x00,0x72,0x49,0xb9,0x30,0x00,0x00,0x00,0xe8,0xc0 } },
  { "title_open", 0x22dce5, 12, 2, { 0x0f,0x2f,0x35,0x94,0x8f,0x4e,0x00,0xf3,0x0f,0x11,0x73,0x18,0x76,0xd6,0x48,0x8b } },
  { "ingame_open", 0x23055b, 7, 6, { 0x0f,0x2f,0x05,0x22,0x67,0x4e,0x00,0x0f,0x86,0xbd,0x00,0x00,0x00,0x83,0xe9,0x01 } },
  { "ingame_disp", 0x230404, 7, 6, { 0x0f,0x2f,0x05,0x79,0x68,0x4e,0x00,0x0f,0x86,0x14,0x02,0x00,0x00,0x40,0x38,0xb7 } },
  { "ingame_exit", 0x230663, 7, 6, { 0x0f,0x2f,0x05,0x1a,0x66,0x4e,0x00,0x0f,0x86,0x83,0x00,0x00,0x00,0xba,0x04,0x00 } },
};
constexpr Gate kRoronaMulti[] = {
  { "title_alloc", 0x22acf1, 7, 2, { 0x0f,0x2f,0x05,0x74,0x43,0x50,0x00,0x72,0x49,0xb9,0x30,0x00,0x00,0x00,0xe8,0xe0 } },
  { "title_open", 0x23a0e5, 12, 2, { 0x0f,0x2f,0x35,0x84,0x4f,0x4f,0x00,0xf3,0x0f,0x11,0x73,0x18,0x76,0xd6,0x48,0x8b } },
  { "ingame_open", 0x23cefb, 7, 6, { 0x0f,0x2f,0x05,0x72,0x21,0x4f,0x00,0x0f,0x86,0xbd,0x00,0x00,0x00,0x83,0xe9,0x01 } },
  { "ingame_disp", 0x23cda4, 7, 6, { 0x0f,0x2f,0x05,0xc9,0x22,0x4f,0x00,0x0f,0x86,0x14,0x02,0x00,0x00,0x40,0x38,0xb7 } },
  { "ingame_exit", 0x23d003, 7, 6, { 0x0f,0x2f,0x05,0x6a,0x20,0x4f,0x00,0x0f,0x86,0x83,0x00,0x00,0x00,0xba,0x04,0x00 } },
};
constexpr Gate kMeruruEn[] = {
  { "title_alloc", 0x1f8cdc, 7, 2, { 0x0f,0x2f,0x05,0xa5,0xf9,0x42,0x00,0x72,0x36,0xb9,0x30,0x00,0x00,0x00,0xe8,0xe5 } },
  { "title_open", 0x2018a4, 12, 2, { 0x0f,0x2f,0x05,0xe1,0x6d,0x42,0x00,0xf3,0x0f,0x11,0x41,0x28,0x76,0x17,0x48,0x8b } },
  { "ingame_open", 0x2014f7, 7, 6, { 0x0f,0x2f,0x05,0x92,0x71,0x42,0x00,0x0f,0x86,0xd3,0x00,0x00,0x00,0x83,0xe9,0x01 } },
  { "ingame_disp", 0x2013e4, 7, 6, { 0x0f,0x2f,0x05,0xa5,0x72,0x42,0x00,0x0f,0x86,0xe6,0x01,0x00,0x00,0x40,0x38,0x77 } },
  { "ingame_exit", 0x20160f, 7, 2, { 0x0f,0x2f,0x05,0x7a,0x70,0x42,0x00,0x76,0x79,0xba,0x04,0x00,0x00,0x00,0x48,0x8d } },
};
constexpr Gate kMeruruMulti[] = {
  { "title_alloc", 0x1e995c, 7, 2, { 0x0f,0x2f,0x05,0x7d,0xaa,0x43,0x00,0x72,0x36,0xb9,0x30,0x00,0x00,0x00,0xe8,0xc5 } },
  { "title_open", 0x1f2694, 12, 2, { 0x0f,0x2f,0x05,0x49,0x1d,0x43,0x00,0xf3,0x0f,0x11,0x41,0x28,0x76,0x17,0x48,0x8b } },
  { "ingame_open", 0x1f22e7, 7, 6, { 0x0f,0x2f,0x05,0xfa,0x20,0x43,0x00,0x0f,0x86,0xd3,0x00,0x00,0x00,0x83,0xe9,0x01 } },
  { "ingame_disp", 0x1f21d4, 7, 6, { 0x0f,0x2f,0x05,0x0d,0x22,0x43,0x00,0x0f,0x86,0xe6,0x01,0x00,0x00,0x40,0x38,0x77 } },
  { "ingame_exit", 0x1f23ff, 7, 2, { 0x0f,0x2f,0x05,0xe2,0x1f,0x43,0x00,0x76,0x79,0xba,0x04,0x00,0x00,0x00,0x48,0x8d } },
};

constexpr Gate kTotoriEn[] = {
  { "title_alloc",  0x27a4cc, 7,  2, { 0x0f,0x2f,0x05,0x11,0x75,0x40,0x00,0x72,0x36,0xb9,0x30,0x00,0x00,0x00,0xe8,0x05 } },
  { "title_open",   0x286c84, 12, 2, { 0x0f,0x2f,0x05,0x61,0xad,0x3f,0x00,0xf3,0x0f,0x11,0x41,0x28,0x76,0x17,0x48,0x8b } },
  { "ingame_open",  0x286947, 7,  6, { 0x0f,0x2f,0x05,0x8a,0xc4,0x3f,0x00,0x0f,0x86,0xe2,0x00,0x00,0x00,0x83,0xe9,0x01 } },
  { "ingame_exit",  0x286a67, 7,  2, { 0x0f,0x2f,0x05,0x6a,0xc3,0x3f,0x00,0x76,0x6b,0xba,0x04,0x00,0x00,0x00,0x48,0x8d } },
};

constexpr Gate kTotoriMulti[] = {
  { "title_alloc",  0x49724c, 7,  2, { 0x0f,0x2f,0x05,0x91,0x08,0x48,0x00,0x72,0x36,0xb9,0x30,0x00,0x00,0x00,0xe8,0x85 } },
  { "title_open",   0x4a3b74, 12, 2, { 0x0f,0x2f,0x05,0x71,0x3f,0x47,0x00,0xf3,0x0f,0x11,0x41,0x28,0x76,0x17,0x48,0x8b } },
  { "ingame_open",  0x4a3837, 7,  6, { 0x0f,0x2f,0x05,0x92,0x78,0x47,0x00,0x0f,0x86,0xe2,0x00,0x00,0x00,0x83,0xe9,0x01 } },
  { "ingame_exit",  0x4a3957, 7,  2, { 0x0f,0x2f,0x05,0x72,0x77,0x47,0x00,0x76,0x6b,0xba,0x04,0x00,0x00,0x00,0x48,0x8d } },
};

// ---- the carried press ------------------------------------------------------
//
// The save data slots view acts on a button's RELEASE, not its press. The first
// thing WinSaveLoadScene::Update does once its own 0.1 s input window expires is
// ask the pad whether button 0xc was just released.
//
// So the press that opens the view is not the problem; the release of that same
// press is. Press confirm, the menu opens the view, keep holding, let go, and
// the view that your press just opened consumes your release as its own confirm
// and shows the load prompt. Vanilla hides this behind the 0.5 s wait in front
// of the view: by the time it exists, you have long since let go. With the wait
// removed the view is up in time to catch the release, and a hold of much over
// 0.1 s triggers it.
//
// The engine's pad state is two 16-byte arrays, current and previous, one byte
// per button, with previous 0x10 above current. The query at the end of the
// gated path is a plain edge test over them:
//
//   mode 0 = cur                 held
//   mode 1 = cur && !prev        just pressed
//   mode 2 = prev && !cur        just released     <- what the view asks for
//
// The repair keeps the edge test honest rather than adding the delay back. On
// the first frame the view is open, every button already down was down before
// the view existed, so no edge belongs to this view. Those buttons are recorded,
// and while each stays down its previous byte is forced to match its current
// one, which makes both edges impossible for it. On the frame it comes up, the
// forced write lands first and lands as zero, so the release edge cannot fire
// either, and the button then stops being tracked. Buttons pressed after the
// view opened are never touched, so the view responds normally to everything
// that is genuinely aimed at it.
//
// This is a real defect in the game, not one the gate removal introduced: the
// same thing happens in vanilla if you hold confirm for over half a second. The
// repair is installed with the gate removal because that is what makes it easy
// to hit, and so that turning the feature off restores vanilla exactly.
struct InputSite {
  Title title;
  uint8_t build;
  uintptr_t updateRva;
  uint32_t openFlagOffset;   // scene byte, nonzero while the view is up
  uintptr_t heldStateRva;    // current-state array; previous sits 0x10 above
  std::array<BYTE, 16> expected;
};

constexpr InputSite kInputSites[] = {
  // title          build                update    open    held state
  { Title::Rorona, BuildEnglish,       0x0669a0, 0x25f8, 0x10e6cb8,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x80, 0xb9, 0xf8, 0x25, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9 } },
  { Title::Rorona, BuildMultilingual,  0x06c7e0, 0x25f8, 0x1123fb8,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x80, 0xb9, 0xf8, 0x25, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9 } },
  { Title::Totori, BuildEnglish,       0x029360, 0x2628, 0x0cddd68,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x80, 0xb9, 0x28, 0x26, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9 } },
  { Title::Totori, BuildMultilingual,  0x244fe0, 0x2628, 0x103f138,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x80, 0xb9, 0x28, 0x26, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9 } },
  { Title::Meruru, BuildEnglish,       0x0cbea0, 0x25f8, 0x0fe7608,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x80, 0xb9, 0xf8, 0x25, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9 } },
  { Title::Meruru, BuildMultilingual,  0x0b7ea0, 0x25f8, 0x1045ae8,
    { 0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x80, 0xb9, 0xf8, 0x25, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9 } },
};

// One byte per button in each array. The engine clears this state as five
// consecutive qwords covering both arrays and a little beyond, so writing
// sixteen entries of the previous-state array stays inside the block the engine
// itself treats as one unit.
constexpr size_t kButtonCount = 16;

using UpdateProc = uint8_t (STDMETHODCALLTYPE*)(uintptr_t);
UpdateProc originalUpdate = nullptr;

BYTE* heldState = nullptr;       // current-state array, kButtonCount bytes
BYTE* previousState = nullptr;   // previous-state array
uint32_t openFlagOffset = 0;

// Which buttons were already down when the view opened, and which scene that
// was. Update is a method, so the same detour serves every instance; keying the
// state to the instance means a second scene cannot inherit the first's carried
// set, and a torn-down scene cannot clear a live one's. One instance is what the
// engine actually constructs, through a lazy singleton, so this costs a pointer
// compare to remove a whole class of question rather than to fix a seen bug.
//
// These are plain values because Update runs on the game's main thread, the same
// one that fills the pad state it reads.
uintptr_t trackedScene = 0;
uint32_t carried = 0;

uint8_t STDMETHODCALLTYPE repairedUpdate(uintptr_t self) {
  const bool open = *reinterpret_cast<const BYTE*>(self + openFlagOffset) != 0;

  if (!open) {
    // Closed. Forget this scene's set, so the next opening starts from the pad
    // as it is at that moment rather than from a stale one. Another scene's
    // tracking is left alone.
    if (trackedScene == self) {
      trackedScene = 0;
      carried = 0;
    }
    return originalUpdate(self);
  }

  if (trackedScene != self) {
    trackedScene = self;
    carried = 0;
    for (size_t i = 0; i < kButtonCount; ++i) {
      if (heldState[i])
        carried |= 1u << i;
    }
  }

  // The previous-state array is the engine's, shared by everything that asks
  // for an edge this frame, so the forced values are put back once the view has
  // read them. Without that, a carried button loses its press and release edges
  // for every consumer that runs after this Update, not just for the query this
  // repair is aimed at.
  uint8_t forced[kButtonCount] = {};
  uint8_t saved[kButtonCount] = {};
  uint32_t doctored = 0;

  for (size_t i = 0; i < kButtonCount; ++i) {
    const uint32_t bit = 1u << i;
    if (!(carried & bit))
      continue;
    // Order matters. Writing previous before testing means the release frame
    // writes a zero, which is what stops the release edge the view acts on.
    saved[i] = previousState[i];
    forced[i] = heldState[i];
    previousState[i] = forced[i];
    doctored |= bit;
    if (!heldState[i])
      carried &= ~bit;
  }

  const uint8_t result = originalUpdate(self);

  // Only bytes still holding what was written are restored. If the engine
  // re-polled the pad during Update, its value is newer than the saved one and
  // putting the old byte back would undo a real input.
  for (size_t i = 0; i < kButtonCount; ++i) {
    if ((doctored & (1u << i)) && previousState[i] == forced[i])
      previousState[i] = saved[i];
  }
  return result;
}

bool installCarriedPressRepair(BYTE* base, const Game& game) {
  const InputSite* site = nullptr;
  for (const InputSite& candidate : kInputSites) {
    if (candidate.title == currentTitle() && candidate.build == game.exeBuild) {
      site = &candidate;
      break;
    }
  }
  if (!site) {
    log("FIXES save_menu_carried_press=not_applicable");
    return false;
  }

  BYTE* update = base + site->updateRva;
  if (!matches(update, site->expected)) {
    log("FIXES save_menu_carried_press=signature_mismatch at 0x", std::hex,
        site->updateRva, std::dec);
    return false;
  }

  // The pad block is a fixed image global the detour writes every frame the
  // view is open, so it gets the install-time writableRange proof mem.h asks
  // for. One range covers both arrays: previous sits 0x10 above current, so the
  // block is 0x10 + kButtonCount bytes from the current-state base. The
  // prologue match above proves the function; this proves the separate
  // per-build array RVA on the same table row.
  if (!writableRange(reinterpret_cast<uintptr_t>(base + site->heldStateRva),
                     0x10 + kButtonCount)) {
    log("FIXES save_menu_carried_press=state_unmapped at 0x", std::hex,
        site->heldStateRva, std::dec);
    return false;
  }

  // The state pointers go in before the hook, because the detour dereferences
  // them on its first call and there is no ordering between the two otherwise.
  heldState = base + site->heldStateRva;
  previousState = heldState + 0x10;
  openFlagOffset = site->openFlagOffset;

  const bool ok = installMinHookDetour(update,
    reinterpret_cast<void*>(&repairedUpdate),
    reinterpret_cast<void**>(&originalUpdate));
  log("FIXES save_menu_carried_press=", ok ? "active" : "failed");
  return ok;
}

// All or nothing. A half-applied set would leave the view reachable through one
// path and gated through another, which is the worst thing to hand someone
// trying to measure whether this helped. PagePatchTransaction retains both the
// original bytes and page protections until the carried-press hook commits.
bool applyGates(BYTE* base, const Gate* gates, size_t count,
                PagePatchTransaction& transaction) {
  std::array<PagePatch, 5> patches = {};
  if (count > patches.size()) {
    log("FIXES save_menu_gates=failed (patch capacity)");
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!matches(base + gates[i].comissRva, gates[i].expected)) {
      log("FIXES save_menu_gates=signature_mismatch at ", gates[i].name,
          " 0x", std::hex, gates[i].comissRva, std::dec, "; nothing patched");
      return false;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    patches[i] = {
      base + gates[i].comissRva + gates[i].branchOffset,
      gates[i].expected.data() + gates[i].branchOffset,
      gates[i].branchLength,
    };
  }
  if (!transaction.applyNops(patches.data(), count)) {
    const PagePatchFailure& failure = transaction.failure();
    log("FIXES save_menu_gates=apply_failed stage=",
        pagePatchStageName(failure.stage), " at ",
        failure.index < count ? gates[failure.index].name : "capacity");
    if (transaction.rollbackFailure().stage != PagePatchStage::None) {
      const PagePatchFailure& rollback = transaction.rollbackFailure();
      log("FIXES save_menu_gates=rollback_incomplete stage=",
          pagePatchStageName(rollback.stage), " at ",
          rollback.index < count ? gates[rollback.index].name : "capacity");
    }
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    log("FIXES save_menu_gate ", gates[i].name, " neutered at 0x", std::hex,
        gates[i].comissRva + gates[i].branchOffset, std::dec, " (",
        static_cast<int>(gates[i].branchLength), " bytes)");
  }
  return true;
}

}  // namespace

bool installSaveMenuFix(BYTE* base, const Game& game) {
  if (featureSupport(Feature::FastSaveMenu) == Support::Unsupported) {
    log("FIXES save_menu_gates=not_applicable");
    return false;
  }
  if (!featureEnabled(Feature::FastSaveMenu)) {
    log("FIXES save_menu_gates=off");
    return false;
  }
  const bool english = game.exeBuild == BuildEnglish;
  const Gate* gates = nullptr;
  size_t count = 0;
  switch (currentTitle()) {
    case Title::Totori:
      gates = english ? kTotoriEn : kTotoriMulti; count = 4; break;
    case Title::Rorona:
      gates = english ? kRoronaEn : kRoronaMulti; count = 5; break;
    case Title::Meruru:
      gates = english ? kMeruruEn : kMeruruMulti; count = 5; break;
    default:
      log("FIXES save_menu_gates=not_applicable"); return false;
  }
  PagePatchTransaction transaction;
  const bool ok = applyGates(base, gates, count, transaction);
  log("FIXES save_menu_gates=", ok ? "active" : "failed");
  if (!ok)
    return false;
  if (!installCarriedPressRepair(base, game)) {
    // The gates must not outlive the repair. With the waits removed the view
    // opens in time to catch the release of the press that opened it, which is
    // the defect the repair exists for, so put the branches back and leave the
    // game behaving as it shipped.
    const bool restored = transaction.rollback();
    log("FIXES save_menu_gates=",
        restored ? "rolled_back" : "rollback_incomplete");
    return false;
  }
  transaction.commit();
  return true;
}

}  // namespace atfix
