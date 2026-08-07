// SPDX-License-Identifier: MIT
//
// Battle-shadow-restore subsystem, split out of menu_fix.cpp. Restores battle and
// cut-in character shadows: it tracks the battle state machine, detects cinematic
// / cut-in states, registers party characters as shadow casters via the engine's
// ShadowCharacterBuild path, and installs the tactical-scene hooks. The menu
// core drives it through two entry points (installBattleShadowRestore,
// battleFrameTick); shared globals gameBase /
// supportedGame come from menu_internal.h. See sync_fix.cpp / battle_shadows.cpp
// for the D3D-side cut-in shadow work this feeds.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "../vendor/minhook/include/MinHook.h"
#include "config.h"        // verboseLogging
#include "game.h"          // featureEnabled, Feature, currentTitle, Title
#include "hook_util.h"     // Game, matches, installDetour, installMinHookDetour
#include "log.h"
#include "mem.h"           // readableRange, tryRead
#include "util.h"          // arlandReturnAddress
#include "menu_internal.h" // gameBase, supportedGame, g_battleActive

namespace atfix {
extern Log log;

// Forward declarations for cross-referenced battle functions (mirrors the decls
// that lived at the top of menu_fix.cpp).
bool arlandInCinematicBattle();
const char* currentBattleState();
bool inActionCutin();


// ==== A: battle typedefs ====
using ShadowCharacterBuildProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using ShadowHelperInitProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using BattleActorInitProc = uintptr_t (*)(uintptr_t, uintptr_t);
using BtlCharaCtorProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);

// ==== B: battle originals + globals ====
ShadowCharacterBuildProc originalShadowCharacterBuild = nullptr;
ShadowHelperInitProc originalShadowHelperInit = nullptr;
BattleActorInitProc originalBattleActorInit = nullptr;
BtlCharaCtorProc originalBtlCharaPartyCtor = nullptr;
BtlCharaCtorProc originalBtlCharaMonsterCtor = nullptr;

struct PendingBattleShadow {
  uintptr_t helper = 0;
  uintptr_t scene = 0;
  uintptr_t character = 0;
};

// Which (helper, character) pairs have already had an actor init observed in
// the current battle. Feeds the deferred= field of the BATTLE_ACTOR_INIT trace.
//
// Cleared at battle end. The key includes the helper, which is embedded in the
// battle game mode and so is a fresh address every battle, meaning an entry from
// an earlier battle can never match again. Keeping them would grow the vector
// for the whole session and lengthen the scan this takes under a mutex on every
// actor init, in exchange for nothing.
std::mutex pendingBattleShadowMutex;
std::vector<PendingBattleShadow> pendingBattleShadows;

void clearPendingBattleShadows() {
  std::lock_guard<std::mutex> lock(pendingBattleShadowMutex);
  pendingBattleShadows.clear();
}

// Battle-shadow reconstruction, enabled by default on the recognized Rorona
// executable (ARLAND_BATTLE_SHADOWS=0 disables). BtlChara-family instances are
// collected as they are constructed; once the battle ShadowHelper finishes
// initializing we register each one as a shadow caster, reproducing the
// slot-45 init (RVA 0x1072a0) the battle flow never dispatches on its characters.
std::mutex battleCharaMutex;
std::vector<uintptr_t> battleCharas;
std::unordered_set<uintptr_t> dispatchedBattleCharas;

std::atomic<uintptr_t> g_battleHelper{0};
std::atomic<uintptr_t> g_battleGameMode{0};
std::atomic<uintptr_t> g_battleScene{0};
std::atomic<uintptr_t> g_battleCharaVectorAddr{0};
std::atomic<uintptr_t> g_savedGlobalHelper{0};
std::atomic<bool> g_battleActive{false};
std::atomic<bool> g_battleContainerFound{false};
std::atomic<bool> g_battleRegistered{false};
std::atomic<uint64_t> g_battleTickCounter{0};
std::atomic<uint32_t> g_battleDeadFrames{0};
// The battle game-mode pointer most recently observed alive (party vector
// holding BtlCharas). The battle-end watchdog only arms for a game-mode it has
// seen alive, so a slow battle intro (party not yet spawned) cannot trip it.
std::atomic<uintptr_t> g_battleSeenLiveMode{0};
std::atomic<uintptr_t> g_battleStateSlot{0};
std::atomic<uintptr_t> g_lastBattleStateVt{0};
std::atomic<uintptr_t> g_lastSceneA{0};
std::atomic<uintptr_t> g_lastSceneB{0};
std::atomic<uintptr_t> g_lastSceneHelper{0};

// ==== C1: gates/traces/tables/regs ====
// Master switch for the whole battle-shadow subsystem (env ARLAND_BATTLE_SHADOWS,
// on unless that says 0). This is deliberately NOT the per-game gate: it also
// enables Meruru's cinematic battle-state detection (installMeruruBattleStateHook
// depends on it), which the cut-in shadow fix needs.
// Per-game ordinary-combat caster restoration is gated separately by
// g_battleAddrs->casterRestore (Rorona only; Totori and Meruru cast natively
// and use this subsystem only for battle tracking and cut-in protection).
//
// There is no ini key. Rorona is missing shadows the engine means to draw, so
// putting them back is a defect correction rather than a preference, and the
// env variable is here for an A/B during development, not for players.
bool battleShadowRestoreEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_BATTLE_SHADOWS");
    return !value || value[0] != '0';
  }();
  return enabled;
}

bool sceneTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_SCENE_TRACE");
    if (value)
      return value[0] != '0';
    // Follows [Diagnostics] VerboseLogging otherwise: a pure observer, so the
    // launcher's checkbox can turn it on without changing what is being
    // observed. The traces that alter behaviour (the menu transition trace, the
    // cut-in blob probe) deliberately do NOT do this -- a diagnostic that moves
    // the code path it reports on is worse than none.
    return atfix::verboseLogging();
  }();
  return enabled;
}

size_t shadowLayerCount(uintptr_t helper, size_t offset);

// BtlChara-family vtable RVAs (ImageBase 0x140000000). A collected pointer is
// only dereferenced for the opt-in dispatch if it still carries one of these,
// which rejects freed/reused objects left over from an earlier battle.
// Same class order in both builds; multilingual values homologue-matched and
// slot-verified.
const uintptr_t kBtlCharaVtableRvasEn[] = {
  0x76e080,  // BtlChara
  0x76e2c0,  // BtlCharaEffect
  0x76e438,  // BtlCharaDummy
  0x76e5b0,  // BtlCharaSynchro
  0x76ec38,  // BtlCharaMonster
  0x76edd8,  // BtlCharaParty
  0x76f088,  // BtlCharaRefractionEffect
  0x76f228,  // BtlCharaRemoteWeapon
};
const uintptr_t kBtlCharaVtableRvasMulti[] = {
  0x78bcb0,  // BtlChara
  0x78bef0,  // BtlCharaEffect
  0x78c068,  // BtlCharaDummy
  0x78c1e0,  // BtlCharaSynchro
  0x78c868,  // BtlCharaMonster
  0x78ca30,  // BtlCharaParty
  0x78cce0,  // BtlCharaRefractionEffect
  0x78ce80,  // BtlCharaRemoteWeapon
};

// Meruru (A13V) BtlChara-family vtables, both builds, located via MSVC RTTI
// (complete-object-locator back-references; method validated by reproducing
// every Rorona EN value first). Meruru has no BtlCharaDummy/BtlCharaSynchro.
const uintptr_t kBtlCharaVtableRvasMeruruEn[] = {
  0x670020,  // BtlChara
  0x6701c8,  // BtlCharaEffect
  0x670498,  // BtlCharaMonster
  0x670600,  // BtlCharaParty
  0x670768,  // BtlCharaRefractionEffect
  0x670330,  // BtlCharaRemoteWeapon
};
const uintptr_t kBtlCharaVtableRvasMeruruMulti[] = {
  0x66c010,  // BtlChara
  0x66c1b8,  // BtlCharaEffect
  0x66c488,  // BtlCharaMonster
  0x66c5f0,  // BtlCharaParty
  0x66c758,  // BtlCharaRefractionEffect
  0x66c320,  // BtlCharaRemoteWeapon
};

// Per-game, per-executable-build addresses used by the battle-shadow machinery
// outside the hook installers. Selected once at detection time. Rorona gets the
// full v0.3 caster restoration (casterRestore); Meruru's engine revision
// registers battle casters natively (per-character model-build path calls
// ShadowCharacterBuild — a call site Rorona lacks), so Meruru only needs the
// battle state tracking that drives the cut-in gate/dim patches in sync_fix.
struct BattleBuildAddrs {
  const uintptr_t* btlCharaVtables;
  size_t btlCharaVtableCount;
  uintptr_t managerSlot;       // [gameBase+managerSlot]+helperSlotOffset = active helper
  uintptr_t battlePublishRet;  // ShadowHelperInit return address, battle setup
  uintptr_t fieldReentryRet;   // ShadowHelperInit return address, field re-entry
  uintptr_t helperSlotOffset;  // active-helper offset inside the scene manager
  uintptr_t helperEmbedOffset; // ShadowHelper embed offset inside the game mode
  uintptr_t partyVectorOffset; // party std::vector<BtlChara*> offset in gameMode
  uintptr_t initFlagOffset;    // BtlChara one-time actor-init flag byte offset
  uintptr_t hideAllRva;        // tactical-scene hideAll(charaMgr, fade)
  uintptr_t showAllRva;        // tactical-scene showAll(charaMgr)
  uintptr_t deferredHideArmRva; // per-actor deferred setVisible arm
  // Model field offsets the front-run reads and writes. Rorona and Meruru
  // share one layout; Totori's differs and NOT by a single constant: the flag
  // bytes sit 0x13 further on and the duration float 0x14, because Totori's
  // struct carries alignment padding where the earlier one does not. Each set
  // is read off that build's own setter and constructor, never inferred from
  // another build's by adding a guessed delta.
  uintptr_t modelVisibilityOffset;   // current-visibility byte
  uintptr_t modelFadePendingOffset;  // fade-pending byte
  uintptr_t modelFadeDurationOffset; // fade-duration float
  bool casterRestore;          // install the full caster-restoration hook set
  // The battle game mode's constructor and destructor, and the vtable both of
  // them install. This is the engine's own answer to "when does a battle begin
  // and end": the mode is heap-allocated fresh per battle by the mode factory
  // and destroyed by the mode manager's remove case, so each of these runs
  // exactly once per battle. Crucially the destructor does not consult map or
  // scene state, so it fires even when the battle returns to a field map that
  // was never unloaded -- the case the frame watchdog below exists to cover.
  uintptr_t battleModeCtorRva;
  uintptr_t battleModeDtorRva;
  uintptr_t battleModeVtable;
  // The field game mode's per-frame update (nspFM::clsFMCore, IGameMode vtable
  // slot 1), and the vtable it comes from. Only the active mode is ticked, so
  // this runs exactly when the field is the thing on screen -- which is the
  // condition under which the field's own shadow helper belongs in the global
  // slot, whether a battle ended a frame ago or an hour ago. Zero on games that
  // never publish a battle helper, since there is nothing to put back.
  uintptr_t fmCoreUpdateRva;
  uintptr_t fmCoreVtable;
  // The battle state machine, a member of the game mode rather than a pointer
  // to one, so this is a literal displacement in the mode constructor: each
  // build's `lea rcx, [mode + offset]` immediately before the call to the state
  // machine's constructor. Read off the disassembly, cross-checked by the call
  // site lying inside that build's own battleModeCtorRva.
  //
  // The machine keeps its states as an MSVC std::deque<State*> at +0x10 with
  // the count at +0x30, and the current state is its back. The game's own mode
  // update reaches it the same way: `cmp qword ptr [machine+0x30], 0` then the
  // deque accessor. That makes the current state five guarded reads away from
  // the game mode, with no search.
  uintptr_t battleStateMachineOffset;
};

constexpr BattleBuildAddrs kRoronaAddrsEn = {
  kBtlCharaVtableRvasEn, std::size(kBtlCharaVtableRvasEn),
  0x10c73c8, 0xfe6e1, 0x397307,
  0x9d0, 0x68, 0x658, 0x2d0, 0x10c2c0, 0x10c270, 0xc5f80,
  0x80, 0x8f, 0x90, true,
  0xfde20, 0xfe120, 0x76d248, 0x2d5fd0, 0x9be9b0,
  0x180,
};
constexpr BattleBuildAddrs kRoronaAddrsMulti = {
  kBtlCharaVtableRvasMulti, std::size(kBtlCharaVtableRvasMulti),
  0x11044c8, 0x106781, 0x3ac8d7,
  0x9d0, 0x68, 0x658, 0x2d0, 0x1143c0, 0x114370, 0xce020,
  0x80, 0x8f, 0x90, true,
  0x105ec0, 0x1061c0, 0x78ae48, 0x2eac20, 0x9b2298,
  0x180,
};
// Meruru: managerSlot/helperSlotOffset read straight from the caster-group
// build's prologue (EN 0x396f80: mov rax,[rip+...]=0xfe0b30; mov r10,[rax+0x960];
// ML 0x394030 -> 0x1040410). battlePublishRet/fieldReentryRet are the two (and
// only) static ShadowHelperInit call sites; the battle one is preceded by
// lea rcx,[r14+0x68] exactly like Rorona's, the field one is followed by the
// group-build call. partyVectorOffset from the BtlCharaMgr embed (gameMode+0x638
// + vector at +0x10; Rorona control run reproduced the known 0x658).
constexpr BattleBuildAddrs kMeruruAddrsEn = {
  kBtlCharaVtableRvasMeruruEn, std::size(kBtlCharaVtableRvasMeruruEn),
  0xfe0b30, 0x119a47, 0x392875,
  0x960, 0x68, 0x648, 0x2c0, 0x1369b0, 0x136940, 0x102cd0,
  0x80, 0x8f, 0x90, false,
  0x118780, 0x119070, 0x66df50, 0, 0,
  0x180,
};
constexpr BattleBuildAddrs kMeruruAddrsMulti = {
  kBtlCharaVtableRvasMeruruMulti, std::size(kBtlCharaVtableRvasMeruruMulti),
  0x1040410, 0x106e97, 0x38f925,
  0x960, 0x68, 0x648, 0x2c0, 0x124080, 0x124010, 0xf0070,
  0x80, 0x8f, 0x90, false,
  0x105bc0, 0x1064b0, 0x669f20, 0, 0,
  0x180,
};
// Totori (EN): static investigation + runtime probe 2026-07-23. Structural
// outlier: the battle helper is EMBEDDED at gameMode+0x60 (not +0x68), there
// is NO global scene-manager/active-helper slot (field and battle each render
// their own helper), and caster registration is native and healthy (probe:
// config byte 1, helper context live before the BtlChara ctors, registry
// fills) — so like Meruru, Totori only needs battle-state tracking for the
// cut-in gate/dim patches. managerSlot/helperSlotOffset/initFlagOffset are 0:
// no such structures exist; the global-slot code paths treat 0 as absent.
// battlePublishRet/fieldReentryRet are the only two static ShadowHelperInit
// (0x1a8930) call sites.
const uintptr_t kBtlCharaVtableRvasTotoriEn[] = {
  0x6dbcf8,  // BtlChara
  0x6dbe88,  // BtlCharaEffect
  0x6dbfd8,  // BtlCharaDummy
  0x6dc128,  // BtlCharaSynchro
  0x6dc3c8,  // BtlCharaMonster
  0x6dc518,  // BtlCharaParty
  0x6dc278,  // BtlCharaRemoteWeapon
};
// The multilingual homologues, RTTI-located. Every entry sits at the same
// offset from BtlChara as its English counterpart (constant delta 0x2a2eb0).
const uintptr_t kBtlCharaVtableRvasTotoriMulti[] = {
  0x97eba8,  // BtlChara
  0x97ed38,  // BtlCharaEffect
  0x97ee88,  // BtlCharaDummy
  0x97efd8,  // BtlCharaSynchro
  0x97f278,  // BtlCharaMonster
  0x97f3c8,  // BtlCharaParty
  0x97f128,  // BtlCharaRemoteWeapon
};
constexpr BattleBuildAddrs kTotoriAddrsEn = {
  kBtlCharaVtableRvasTotoriEn, std::size(kBtlCharaVtableRvasTotoriEn),
  0, 0x1512f0, 0x94212,
  0, 0x60, 0x5f8, 0, 0x170cb0, 0x170c30, 0x133880,
  0x90, 0xa2, 0xa4, false,
  0x14d0e0, 0x14f790, 0x6d9620, 0, 0,
  0xc0,
};

// Totori multilingual. Every vtable here is RTTI-located and every function
// homologue-matched from the English build; both methods were checked by
// reproducing this game's committed English values exactly before being pointed
// at this binary. The battle gate was then confirmed live in-game: mode ctor and
// dtor both fired, the vtable check passed, and the states logged by name.
//
// The ShadowHelperInit publish/re-entry return addresses are required even
// though Totori has no global helper slot to restore: the battle-setup call
// site is what publishes g_battleHelper, and that is the handle the whole
// caster registry walk uses (clearBattleSnodeFlags and friends read the node
// vector at helper+0x48). Without it the tactical hooks install and then clear
// nothing. Both were located from the two callers of ShadowHelperInit, matched
// to their English counterparts at 0.994 and 0.979 similarity with identical
// lengths. Struct offsets are shared, being one source compiled twice.
constexpr BattleBuildAddrs kTotoriAddrsMulti = {
  kBtlCharaVtableRvasTotoriMulti, std::size(kBtlCharaVtableRvasTotoriMulti),
  0, 0x36e150, 0x2b08f2,
  0, 0x60, 0x5f8, 0, 0x38db20, 0x38daa0, 0x3506e0,
  0x90, 0xa2, 0xa4, false,
  0x369f40, 0x36c5f0, 0x97b7f0, 0, 0,
  0xc0,
};

// Null until a battle-capable build is recognized; battle-shadow code paths
// treat that as "feature unavailable".
const BattleBuildAddrs* g_battleAddrs = nullptr;

// Battle state-machine vtables (RVA, ImageBase 0x140000000) → name. The current
// state's Update (vtable slot 1) is what runs each frame; recognizing the state
// object lets us log exactly when the attack cut-in (ExecCommand) and victory
// (ResultStart) are active without needing manual F8 marks. Same state order in
// both builds; multilingual values homologue-matched.
struct BattleStateEntry { uintptr_t rva; const char* name; };
const BattleStateEntry kBattleStatesEn[] = {
  {0x76d9a0, "Enter"}, {0x76dd60, "StartWait"}, {0x76da40, "SelectCommand"},
  {0x76da90, "SelectTarget"}, {0x76d8c8, "SelectSkill"}, {0x76d830, "SelectItem"},
  {0x76d798, "SelectDefence"}, {0x76dbd0, "WaitAction"}, {0x76dae0, "ExecCommand"},
  {0x76dc70, "Reaction"}, {0x76db80, "ReactionSkillBefore"},
  {0x76db30, "HelpSkillBefore"}, {0x76dc20, "HelpSkillAfter"},
  {0x76d9f0, "ChangeActiveChara"}, {0x76dcc0, "EndCheck"},
  {0x76ddb0, "TurnEventWait"}, {0x76de00, "EndWait"}, {0x76dea0, "AfterBattle"},
  {0x76dd10, "DeadBoss"}, {0x76de50, "ResultStart"}, {0x76d570, "ResultCountExp"},
  {0x76d630, "ResultDropItem"}, {0x76d6a8, "ResultLevelUp"},
};
const BattleStateEntry kBattleStatesMulti[] = {
  {0x78b5d0, "Enter"}, {0x78b990, "StartWait"}, {0x78b670, "SelectCommand"},
  {0x78b6c0, "SelectTarget"}, {0x78b4f0, "SelectSkill"}, {0x78b458, "SelectItem"},
  {0x78b398, "SelectDefence"}, {0x78b800, "WaitAction"}, {0x78b710, "ExecCommand"},
  {0x78b8a0, "Reaction"}, {0x78b7b0, "ReactionSkillBefore"},
  {0x78b760, "HelpSkillBefore"}, {0x78b850, "HelpSkillAfter"},
  {0x78b620, "ChangeActiveChara"}, {0x78b8f0, "EndCheck"},
  {0x78b9e0, "TurnEventWait"}, {0x78ba30, "EndWait"}, {0x78bad0, "AfterBattle"},
  {0x78b940, "DeadBoss"}, {0x78ba80, "ResultStart"}, {0x78b170, "ResultCountExp"},
  {0x78b230, "ResultDropItem"}, {0x78b2a8, "ResultLevelUp"},
};
// Meruru (A13V) battle states, both builds, located via MSVC RTTI — Meruru
// ships full .?AVGmStateBtl*@@ type descriptors, so each vtable was resolved
// from its complete-object locator (the locator method reproduced all 23
// Rorona EN entries above exactly before being applied to Meruru). Same state
// names as Rorona, so isCinematicState applies unchanged.
const BattleStateEntry kBattleStatesMeruruEn[] = {
  {0x66f268, "Enter"}, {0x66f718, "StartWait"}, {0x66f308, "SelectCommand"},
  {0x66f448, "SelectTarget"}, {0x66f358, "SelectSkill"}, {0x66f3a8, "SelectItem"},
  {0x66f3f8, "SelectDefence"}, {0x66f588, "WaitAction"}, {0x66f498, "ExecCommand"},
  {0x66f628, "Reaction"}, {0x66f538, "ReactionSkillBefore"},
  {0x66f4e8, "HelpSkillBefore"}, {0x66f5d8, "HelpSkillAfter"},
  {0x66f2b8, "ChangeActiveChara"}, {0x66f678, "EndCheck"},
  {0x66f768, "TurnEventWait"}, {0x66f7b8, "EndWait"}, {0x66f948, "AfterBattle"},
  {0x66f6c8, "DeadBoss"}, {0x66f808, "ResultStart"}, {0x66f858, "ResultCountExp"},
  {0x66f8f8, "ResultDropItem"}, {0x66f8a8, "ResultLevelUp"},
};
const BattleStateEntry kBattleStatesMeruruMulti[] = {
  {0x66b260, "Enter"}, {0x66b710, "StartWait"}, {0x66b300, "SelectCommand"},
  {0x66b440, "SelectTarget"}, {0x66b350, "SelectSkill"}, {0x66b3a0, "SelectItem"},
  {0x66b3f0, "SelectDefence"}, {0x66b580, "WaitAction"}, {0x66b490, "ExecCommand"},
  {0x66b620, "Reaction"}, {0x66b530, "ReactionSkillBefore"},
  {0x66b4e0, "HelpSkillBefore"}, {0x66b5d0, "HelpSkillAfter"},
  {0x66b2b0, "ChangeActiveChara"}, {0x66b670, "EndCheck"},
  {0x66b760, "TurnEventWait"}, {0x66b7b0, "EndWait"}, {0x66b940, "AfterBattle"},
  {0x66b6c0, "DeadBoss"}, {0x66b800, "ResultStart"}, {0x66b850, "ResultCountExp"},
  {0x66b8f0, "ResultDropItem"}, {0x66b8a0, "ResultLevelUp"},
};
// Totori (A12V EN) battle states via the same RTTI locator method. 22 states:
// no SelectDefence, and the result chain is renamed (Result/AddPay/DropItem/
// LvUp instead of ResultStart/ResultCountExp/ResultDropItem/ResultLevelUp);
// isCinematicState carries the Totori spellings. The corresponding multilingual
// table follows below.
const BattleStateEntry kBattleStatesTotoriEn[] = {
  {0x6daeb0, "Enter"}, {0x6db310, "StartWait"}, {0x6daf50, "SelectCommand"},
  {0x6db040, "SelectTarget"}, {0x6dafa0, "SelectSkill"},
  {0x6daff0, "SelectItem"}, {0x6db180, "WaitAction"},
  {0x6db090, "ExecCommand"}, {0x6db220, "Reaction"},
  {0x6db130, "ReactionSkillBefore"}, {0x6db0e0, "HelpSkillBefore"},
  {0x6db1d0, "HelpSkillAfter"}, {0x6daf00, "ChangeActiveChara"},
  {0x6db270, "EndCheck"}, {0x6db360, "TurnEventWait"}, {0x6db3b0, "EndWait"},
  {0x6db540, "AfterBattle"}, {0x6db2c0, "DeadBoss"}, {0x6db400, "Result"},
  {0x6db450, "AddPay"}, {0x6db4f0, "DropItem"}, {0x6db4a0, "LvUp"},
};
// Totori multilingual, same locator and the same 22 states in the same order.
// Every entry sits at the same offset from Enter as its English homologue
// (constant delta 0x2a2ed0), so the table is one relocation of one layout.
const BattleStateEntry kBattleStatesTotoriMulti[] = {
  {0x97dd80, "Enter"}, {0x97e1e0, "StartWait"}, {0x97de20, "SelectCommand"},
  {0x97df10, "SelectTarget"}, {0x97de70, "SelectSkill"},
  {0x97dec0, "SelectItem"}, {0x97e050, "WaitAction"},
  {0x97df60, "ExecCommand"}, {0x97e0f0, "Reaction"},
  {0x97e000, "ReactionSkillBefore"}, {0x97dfb0, "HelpSkillBefore"},
  {0x97e0a0, "HelpSkillAfter"}, {0x97ddd0, "ChangeActiveChara"},
  {0x97e140, "EndCheck"}, {0x97e230, "TurnEventWait"}, {0x97e280, "EndWait"},
  {0x97e410, "AfterBattle"}, {0x97e190, "DeadBoss"}, {0x97e2d0, "Result"},
  {0x97e320, "AddPay"}, {0x97e3c0, "DropItem"}, {0x97e370, "LvUp"},
};
const BattleStateEntry* g_battleStates = nullptr;
size_t g_battleStateCount = 0;

bool isBattleCharaVtable(uintptr_t vtable) {
  if (!gameBase || !vtable || !g_battleAddrs)
    return false;
  const uintptr_t rva = vtable - reinterpret_cast<uintptr_t>(gameBase);
  for (size_t i = 0; i < g_battleAddrs->btlCharaVtableCount; i++)
    if (rva == g_battleAddrs->btlCharaVtables[i])
      return true;
  return false;
}

void recordBattleChara(uintptr_t chara) {
  if (!chara)
    return;
  std::lock_guard<std::mutex> lock(battleCharaMutex);
  if (std::find(battleCharas.begin(), battleCharas.end(), chara) ==
      battleCharas.end())
    battleCharas.push_back(chara);
}

uintptr_t tracedBtlCharaPartyCtor(uintptr_t self, uintptr_t a2, uintptr_t a3,
                                  uintptr_t a4) {
  const uintptr_t result = originalBtlCharaPartyCtor(self, a2, a3, a4);
  recordBattleChara(self);
  return result;
}

uintptr_t tracedBtlCharaMonsterCtor(uintptr_t self, uintptr_t a2, uintptr_t a3,
                                    uintptr_t a4) {
  const uintptr_t result = originalBtlCharaMonsterCtor(self, a2, a3, a4);
  recordBattleChara(self);
  return result;
}

// Register the collected battle characters as shadow casters. The battle
// ShadowHelper lives embedded in the game mode at gameMode+0x68, so gameMode is
// helper-0x68; only characters that belong to this game mode, still hold their
// one-time init flag clear, and expose a model sub-object are registered. The
// registration is exactly the ShadowCharacterBuild(helper, scene, character)
// call that slot 45 makes internally, with character = [chara+0x18]; scene is
// the helper-init resource argument, which the field path proves equals the
// scene ShadowCharacterBuild expects.
size_t dispatchBattleCharaShadows(uintptr_t helper, uintptr_t scene) {
  const uintptr_t gameMode = helper && g_battleAddrs
    ? helper - g_battleAddrs->helperEmbedOffset : 0;
  const bool contextLive = helper &&
    *reinterpret_cast<const uintptr_t*>(helper + 0x18) != 0;
  size_t dispatched = 0;
  size_t candidates = 0;
  {
    std::lock_guard<std::mutex> lock(battleCharaMutex);
    candidates = battleCharas.size();
    if (originalShadowCharacterBuild && scene && contextLive) {
      for (uintptr_t chara : battleCharas) {
        if (!chara || dispatchedBattleCharas.count(chara))
          continue;
        // The list is not cleared between battles, so an entry can name an
        // object the engine has already freed. readableRange first, exactly as
        // registerBattleCharaShadows does before the same dereference; the
        // vtable test alone would happily pass on recycled bytes.
        if (!readableRange(chara, 0x20))
          continue;
        const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(chara);
        if (!isBattleCharaVtable(vtable))
          continue;
        if (*reinterpret_cast<const uintptr_t*>(chara + 0x10) != gameMode)
          continue;
        if (*reinterpret_cast<const uint8_t*>(
              chara + g_battleAddrs->initFlagOffset) != 0)
          continue;
        const uintptr_t character =
          *reinterpret_cast<const uintptr_t*>(chara + 0x18);
        if (!character)
          continue;
        const size_t before = shadowLayerCount(helper, 0x48);
        originalShadowCharacterBuild(helper, scene, character, 0);
        const size_t after = shadowLayerCount(helper, 0x48);
        dispatchedBattleCharas.insert(chara);
        ++dispatched;
        if (sceneTraceEnabled())
          atfix::log("BATTLE_SHADOW_DISPATCH chara=",
            reinterpret_cast<void*>(chara), " vtable_rva=0x", std::hex,
            vtable - reinterpret_cast<uintptr_t>(gameBase), std::dec,
            " character=", reinterpret_cast<void*>(character),
            " registry_before=", before, " registry_after=", after);
      }
    }
  }
  if (sceneTraceEnabled())
    atfix::log("BATTLE_SHADOW_SCAN helper=", reinterpret_cast<void*>(helper),
      " gamemode=", reinterpret_cast<void*>(gameMode),
      " scene=", reinterpret_cast<void*>(scene),
      " context_live=", contextLive,
      " candidates=", candidates, " dispatched=", dispatched);
  return dispatched;
}


// ==== C2: registration + tactical + traced hooks ====

// Scan an object's memory for a std::vector<BtlChara*> — a (begin,end) pair
// whose first element carries a known BtlChara-family vtable — recursing one
// pointer level. Every access is VirtualQuery-guarded so wild members are safe.
size_t scanForBattleCharaVectors(uintptr_t obj, size_t window, int depth,
                                 std::unordered_set<uintptr_t>& seen,
                                 size_t& budget) {
  if (!obj || (obj & 7) || budget == 0 || !seen.insert(obj).second)
    return 0;
  --budget;
  if (!readableRange(obj, window))
    return 0;
  size_t found = 0;
  for (size_t off = 0; off + 0x10 <= window; off += 8) {
    const uintptr_t begin = *reinterpret_cast<const uintptr_t*>(obj + off);
    const uintptr_t end = *reinterpret_cast<const uintptr_t*>(obj + off + 8);
    if (begin && end > begin && (end - begin) <= 0x1000 &&
        (end - begin) % sizeof(uintptr_t) == 0 &&
        readableRange(begin, end - begin)) {
      const uintptr_t elem0 = *reinterpret_cast<const uintptr_t*>(begin);
      if (readableRange(elem0, sizeof(uintptr_t)) &&
          isBattleCharaVtable(*reinterpret_cast<const uintptr_t*>(elem0))) {
        g_battleCharaVectorAddr.store(obj + off, std::memory_order_release);
        if (sceneTraceEnabled()) {
          const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(elem0);
          atfix::log("BATTLE_CONTAINER obj=", reinterpret_cast<void*>(obj),
            " offset=0x", std::hex, off, std::dec,
            " count=", (end - begin) / sizeof(uintptr_t),
            " elem0=", reinterpret_cast<void*>(elem0),
            " vtable_rva=0x", std::hex,
            vt - reinterpret_cast<uintptr_t>(gameBase), std::dec);
        }
        ++found;
      }
    }
    if (depth > 0) {
      const uintptr_t ptr = *reinterpret_cast<const uintptr_t*>(obj + off);
      found += scanForBattleCharaVectors(
        ptr, 0x400, depth - 1, seen, budget);
    }
  }
  return found;
}

// Scan the battle game-mode and scene (two pointer levels) for the party's
// BtlChara vector, logging any hit. Returns the number of vectors found.
size_t locateBattleCharaContainer(uintptr_t gameMode, uintptr_t scene,
                                  const char* phase) {
  std::unordered_set<uintptr_t> seen;
  size_t budget = 2000;
  size_t found = scanForBattleCharaVectors(
    gameMode, 0x1000, 2, seen, budget);
  if (scene)
    found += scanForBattleCharaVectors(
      scene, 0x1000, 2, seen, budget);
  if (sceneTraceEnabled())
    atfix::log("BATTLE_CONTAINER_SCAN phase=", phase,
      " gamemode=", reinterpret_cast<void*>(gameMode),
      " scene=", reinterpret_cast<void*>(scene),
      " found=", found, " objects_scanned=", 2000 - budget);
  return found;
}

// Address of the manager's active-helper slot
// ([manager global]+helperSlotOffset: 0x9d0 Rorona, 0x960 Meruru), or null.
// The manager global RVA is per-game/per-build. On Rorona it was decoded from
// the scene pass's active-helper load; on Meruru it was decoded from the
// caster-group build's corresponding manager load.
uintptr_t* globalActiveHelperSlot() {
  // managerSlot == 0 marks a game with no global helper slot (Totori).
  if (!gameBase || !g_battleAddrs || !g_battleAddrs->managerSlot)
    return nullptr;
  const uintptr_t manager =
    *reinterpret_cast<uintptr_t*>(gameBase + g_battleAddrs->managerSlot);
  if (!manager)
    return nullptr;
  // Every caller stores through this pointer, so prove the slot is writable
  // rather than only that the manager is non-null. sceneIdentityTick guards its
  // read of the same field the same way, and the snode stores in this file all
  // use writableRange. Returning null is the outcome Totori already produces on
  // every call, so no caller needs a new branch.
  const uintptr_t slot = manager + g_battleAddrs->helperSlotOffset;
  if (!writableRange(slot, sizeof(uintptr_t)))
    return nullptr;
  return reinterpret_cast<uintptr_t*>(slot);
}

// Register the located battle party as shadow casters. For each BtlChara in the
// game-mode's character vector, call ShadowCharacterBuild(helper, scene,
// [chara+0x18]) — the same registration the field path performs per character —
// so the renderer that already binds the battle depth targets has casters to
// draw. Runs once per battle. Every access is VirtualQuery-guarded.
void registerBattleCharaShadows() {
  if (g_battleRegistered.load(std::memory_order_acquire) ||
      !originalShadowCharacterBuild)
    return;
  const uintptr_t vecAddr =
    g_battleCharaVectorAddr.load(std::memory_order_acquire);
  if (!vecAddr || !readableRange(vecAddr, 0x10))
    return;
  const uintptr_t begin = *reinterpret_cast<const uintptr_t*>(vecAddr);
  const uintptr_t end = *reinterpret_cast<const uintptr_t*>(vecAddr + 8);
  if (!begin || end <= begin || (end - begin) > 0x1000 ||
      (end - begin) % sizeof(uintptr_t) || !readableRange(begin, end - begin))
    return;

  const uintptr_t helper = g_battleHelper.load(std::memory_order_acquire);
  const uintptr_t scene = g_battleScene.load(std::memory_order_acquire);
  if (!helper || !scene)
    return;

  g_battleRegistered.store(true, std::memory_order_release);
  size_t registered = 0;
  for (uintptr_t p = begin; p < end; p += sizeof(uintptr_t)) {
    const uintptr_t chara = *reinterpret_cast<const uintptr_t*>(p);
    if (!readableRange(chara, 0x20) ||
        !isBattleCharaVtable(*reinterpret_cast<const uintptr_t*>(chara)))
      continue;
    const uintptr_t character = *reinterpret_cast<const uintptr_t*>(chara + 0x18);
    if (!character)
      continue;
    const size_t before = shadowLayerCount(helper, 0x48);
    originalShadowCharacterBuild(helper, scene, character, 0);
    const size_t after = shadowLayerCount(helper, 0x48);
    ++registered;
    if (sceneTraceEnabled())
      atfix::log("BATTLE_SHADOW_REGISTER which=battle",
        " helper=", reinterpret_cast<void*>(helper),
        " chara=", reinterpret_cast<void*>(chara),
        " character=", reinterpret_cast<void*>(character),
        " registry_before=", before, " registry_after=", after);
  }

  // Registering into the battle helper only matters if the renderer traverses
  // it, so publish it into the global slot (saving the field helper to restore
  // on field re-entry). The global-helper target is already the rendered one.
  bool published = false;
  bool republished = false;
  if (uintptr_t* slot = globalActiveHelperSlot()) {
    // Save the displaced helper only on the FIRST publish of a battle. A
    // second publish would otherwise record the battle helper as the thing to
    // put back, and restoring that leaves the field rendering through a
    // battle helper -- which is a field with no shadows, for the rest of the
    // visit. Every later publish still updates the slot, just not the memory
    // of what was there before it.
    uintptr_t nothingSaved = 0;
    if (g_savedGlobalHelper.compare_exchange_strong(nothingSaved, *slot,
          std::memory_order_acq_rel, std::memory_order_acquire))
      published = true;
    else
      republished = true;
    *slot = helper;
  }
  if (sceneTraceEnabled())
    atfix::log("BATTLE_SHADOW_REGISTER_SUMMARY which=battle",
      " helper=", reinterpret_cast<void*>(helper),
      " scene=", reinterpret_cast<void*>(scene), " registered=", registered,
      " published=", published, " republished=", republished,
      " saved=", reinterpret_cast<void*>(
        g_savedGlobalHelper.load(std::memory_order_acquire)));
}

// Cut-in probe: during WaitAction the engine clears bit 0x10000 at
// +0xc0 of selected registered shadow nodes — its per-node caster kill-switch
// for the action camera. Re-set the bit on our registered battle casters so
// the engine itself rebuilds their caster state. Runs on the game's render
// thread; every access is VirtualQuery-guarded.
void restoreBattleSnodeFlags(const char* site) {
  const uintptr_t battleHelper = g_battleHelper.load(std::memory_order_acquire);
  if (!battleHelper || !readableRange(battleHelper + 0x48, 0x10))
    return;
  const uintptr_t rb = *reinterpret_cast<const uintptr_t*>(battleHelper + 0x48);
  const uintptr_t re = *reinterpret_cast<const uintptr_t*>(battleHelper + 0x50);
  if (!rb || re <= rb || (re - rb) > 0x200 || !readableRange(rb, re - rb))
    return;
  // The disable wrapper (0x552aa0) clears BOTH byte +0xC2 (the caster
  // flag) and byte +0xBC (low byte of the per-pass stamp dword). Restore both:
  // flag set, stamp copied from the healthiest sibling in the same registry.
  uint32_t restored = 0, nodes = 0, stamped = 0;
  uint32_t bestStamp = 0;
  for (uintptr_t p = rb; p < re; p += sizeof(uintptr_t)) {
    const uintptr_t snode = *reinterpret_cast<const uintptr_t*>(p);
    if (!snode || !readableRange(snode, 0xc4))
      continue;
    const uint32_t f = *reinterpret_cast<const uint32_t*>(snode + 0xc0);
    if (f & 0x10000)
      bestStamp = std::max(bestStamp,
        *reinterpret_cast<const uint32_t*>(snode + 0xbc));
  }
  for (uintptr_t p = rb; p < re; p += sizeof(uintptr_t)) {
    const uintptr_t snode = *reinterpret_cast<const uintptr_t*>(p);
    if (!snode || !writableRange(snode, 0xc4))
      continue;
    ++nodes;
    auto* flag = reinterpret_cast<uint32_t*>(snode + 0xc0);
    auto* stamp = reinterpret_cast<uint32_t*>(snode + 0xbc);
    if ((*flag & 0x10000) == 0) {
      *flag |= 0x10000;
      ++restored;
    }
    if (bestStamp && *stamp < bestStamp) {
      *stamp = bestStamp;
      ++stamped;
    }
  }
  if (sceneTraceEnabled()) {
    static std::atomic<uint64_t> tick{0};
    static std::atomic<uint32_t> lastRestored{0xffffffff};
    const uint64_t t = tick.fetch_add(1, std::memory_order_relaxed);
    if (restored != lastRestored.exchange(restored) || (t % 300) == 0)
      atfix::log("BATTLE_SNODE_RESTORE site=", site, " restored=", restored,
        " stamped=", stamped, " nodes=", nodes,
        " state=", currentBattleState() ? currentBattleState() : "?",
        " tick=", t);
  }
}

// ---- tactical-scene caster clear (stray-shadow fix, engine-cooperative) ----
// The engine clears the juggled non-focus battlers' caster flags only ~0.25 s
// AFTER the cut-in hide starts (deferred with the visual cross-fade) and
// restores them INSTANTLY at exit — vanilla's dim fade covers both stale
// windows by closing the reception gate. When the mod holds brightness/
// reception from the first fade frame (to remove the visible dim ride-down),
// that cover is gone, so the mod front-runs the engine instead: hooks on the
// tactical-scene hideAll/showAll wrappers clear the registered casters' flags
// immediately at hide, re-clear them at show (undoing the engine's instant
// restore), and restore them shortly after the juggle settles. Visible cut-in
// participants get their flags re-set by the event system's own immediate
// show paths, so the focus actor keeps its shadow.
std::atomic<bool> g_tacticalHooksActive{false};
// Set only when the per-actor deferred-hide front-run installs. Distinct from
// g_tacticalHooksActive: that one covers hideAll/showAll, which do not touch
// the per-actor fade, so it does not license skipping the settle cover.
std::atomic<bool> g_deferredHideArmActive{false};
// Set the first time the front-run actually force-expires a fade, so the log can
// distinguish "hook installed" from "hook did something".
std::atomic<bool> g_deferredHideArmFired{false};
std::atomic<uint64_t> g_snodeRestoreDeadlineMs{0};

// Inverse of restoreBattleSnodeFlags: clear the caster bit on every
// registered battle snode. Same guarded walk; +0xbc stamps are untouched.
void clearBattleSnodeFlags(const char* site) {
  const uintptr_t battleHelper = g_battleHelper.load(std::memory_order_acquire);
  if (!battleHelper || !readableRange(battleHelper + 0x48, 0x10))
    return;
  const uintptr_t rb = *reinterpret_cast<const uintptr_t*>(battleHelper + 0x48);
  const uintptr_t re = *reinterpret_cast<const uintptr_t*>(battleHelper + 0x50);
  if (!rb || re <= rb || (re - rb) > 0x200 || !readableRange(rb, re - rb))
    return;
  uint32_t cleared = 0, nodes = 0;
  for (uintptr_t p = rb; p < re; p += sizeof(uintptr_t)) {
    const uintptr_t snode = *reinterpret_cast<const uintptr_t*>(p);
    if (!snode || !writableRange(snode, 0xc4))
      continue;
    ++nodes;
    auto* flag = reinterpret_cast<uint32_t*>(snode + 0xc0);
    if (*flag & 0x10000) {
      *flag &= ~0x10000u;
      ++cleared;
    }
  }
  if (sceneTraceEnabled())
    atfix::log("CUTIN_SNODE_CLEAR site=", site, " cleared=", cleared,
      " nodes=", nodes,
      " state=", currentBattleState() ? currentBattleState() : "?");
}


using TacticalSceneProc = uintptr_t (*)(uintptr_t, uintptr_t,
                                        uintptr_t, uintptr_t);
TacticalSceneProc originalTacticalHideAll = nullptr;
TacticalSceneProc originalTacticalShowAll = nullptr;

// Per-actor deferred hide arm: within a cut-in the event choreography hides
// individual actors through a deferred setVisible (alpha fade ~0.25 s, caster
// flags cleared by the engine only at fade END). Vanilla's closed reception
// gate covered that window; with the mod's hold open, the full-strength shadow
// would outlive the fading character. Front-run it: on a HIDE arm during a
// cinematic state, clear the actor's subtree caster flags at fade START.
// Shows re-set flags through the engine's immediate paths, so arriving actors
// are unaffected. The visibility object holds its parts vector at +0x28/+0x30;
// the model root is [firstPart+0x10] (mirrors the engine's own expiry path).
using DeferredHideArmProc = uintptr_t (*)(uintptr_t, uintptr_t,
                                          float, uintptr_t);
DeferredHideArmProc originalDeferredHideArm = nullptr;

uintptr_t tracedDeferredHideArm(uintptr_t obj, uintptr_t target,
                                float fade, uintptr_t d) {
  const uintptr_t result = originalDeferredHideArm(obj, target, fade, d);
  // Force-expiry fix (static RE 2026-07-23). The battle caster registry at
  // helper+0x48 holds model LOCATOR ROOTS, not the drawable shadow leaves, so
  // clearing their +0xC2 never affected the shadow map — the shadow pass walks
  // straight through a cleared root to the leaves, which keep casting. A
  // non-focus battler hidden mid-cut-in therefore keeps its shadow until the
  // engine's own alpha-fade expiry (~0.25 s) recursively hides the whole model
  // subtree, including the shadow leaves — that lag is the stray shadow. The
  // fix forces that expiry to happen on the next frame: when a hide (target 0)
  // latches on this Model (fade-pending == 1, i.e. the arm did not early-out)
  // during a cinematic state, zero the fade duration. The engine's own visTick
  // then performs the complete, bookkeeping-correct hide (subtree setVisibility
  // via 0xb9720 plus the +0x8d/+0x8f state the cancel/show path depends on), so
  // there are ZERO manual node writes and the focus actor is untouched (the
  // enumerator never arms it). Cost: the hidden battler pops rather than fading,
  // off-camera and cosmetically negligible.
  // inActionCutin() (NOT arlandInCinematicBattle) — restricted to the mid-
  // battle action cut-ins, excluding the result/victory teardown states where
  // force-expiring would hit the field transition (black-screen risk).
  // Offsets come from the per-build table, never hardcoded: Totori's Model puts
  // the pending byte at +0xa2 and the duration float at +0xa4, so unlike
  // Rorona's +0x8f/+0x90 they are not adjacent (alignment padding at +0xa3) and
  // each needs its own readability check rather than one combined range.
  const uintptr_t pendingOffset = g_battleAddrs->modelFadePendingOffset;
  const uintptr_t durationOffset = g_battleAddrs->modelFadeDurationOffset;
  if ((target & 0xff) == 0 &&
      g_battleActive.load(std::memory_order_acquire) &&
      g_tacticalHooksActive.load(std::memory_order_acquire) &&
      inActionCutin() &&
      pendingOffset && durationOffset &&
      readableRange(obj + pendingOffset, 1) &&
      writableRange(obj + durationOffset, sizeof(float)) &&
      *reinterpret_cast<const uint8_t*>(obj + pendingOffset) == 1) {
    *reinterpret_cast<float*>(obj + durationOffset) = 0.0f;
    // Report the first real force-expiry, not just that the hook installed.
    // "Deferred-hide arm hook installed=1" is true even when the subsystem goes
    // on to do nothing, which is exactly how a silently inert caster clear cost
    // a day of debugging on 2026-07-26.
    if (!g_deferredHideArmFired.exchange(true, std::memory_order_acq_rel) &&
        sceneTraceEnabled())
      atfix::log("Deferred-hide arm force-expired a caster fade (first hit)");
  }
  return result;
}

uintptr_t tracedTacticalHideAll(uintptr_t a, uintptr_t b,
                                uintptr_t c, uintptr_t d) {
  const uintptr_t result = originalTacticalHideAll(a, b, c, d);
  // Both halves of the pair or neither. Only tracedTacticalShowAll sets a
  // restore deadline, so clearing here with showAll missing would drop every
  // caster's shadow for the rest of the session with no path back. The
  // deferred-hide detour above gates on the same flag for the same reason.
  if (g_battleActive.load(std::memory_order_acquire) &&
      g_tacticalHooksActive.load(std::memory_order_acquire)) {
    clearBattleSnodeFlags("hide_all");
    g_snodeRestoreDeadlineMs.store(0, std::memory_order_release);
  }
  return result;
}

uintptr_t tracedTacticalShowAll(uintptr_t a, uintptr_t b,
                                uintptr_t c, uintptr_t d) {
  const uintptr_t result = originalTacticalShowAll(a, b, c, d);
  if (g_battleActive.load(std::memory_order_acquire)) {
    // The original just restored every caster flag while positions may still
    // be mid-restore; re-clear and restore after the juggle settles.
    clearBattleSnodeFlags("show_all");
    g_snodeRestoreDeadlineMs.store(GetTickCount64() + 300,
      std::memory_order_release);
  }
  return result;
}

std::atomic<uint32_t> g_sceneGeneration{0};

uintptr_t tracedShadowHelperInit(uintptr_t helper, uintptr_t id,
                                 uintptr_t resource, uintptr_t config) {
  const uintptr_t caller = reinterpret_cast<uintptr_t>(
    arlandReturnAddress());
  const uintptr_t callerRva = gameBase && caller >= uintptr_t(gameBase)
    ? caller - uintptr_t(gameBase) : 0;
  const uintptr_t result = originalShadowHelperInit(
    helper, id, resource, config);
  // Any shadow-helper init is a scene (re)build — field re-entry OR
  // battle setup. Bump the generation so the D3D layer drops cross-scene
  // caches (light-VP, proxy pairings, recordings) that reference freed
  // geometry from the previous scene.
  if (g_battleAddrs && (callerRva == g_battleAddrs->battlePublishRet ||
                        callerRva == g_battleAddrs->fieldReentryRet))
    g_sceneGeneration.fetch_add(1, std::memory_order_release);
  // Track which helper the render path should use: the battle-setup call site
  // publishes the battle helper, the field re-entry call site hands control
  // back to the field helper. Both return addresses are per-build (EN
  // 0xfe6e1/0x397307, multi 0x106781/0x3ac8d7).
  if (g_battleAddrs && callerRva == g_battleAddrs->battlePublishRet) {
    const uintptr_t gameMode =
      helper ? helper - g_battleAddrs->helperEmbedOffset : 0;
    g_battleHelper.store(helper, std::memory_order_release);
    g_battleGameMode.store(gameMode, std::memory_order_release);
    g_battleScene.store(resource, std::memory_order_release);
    g_battleContainerFound.store(false, std::memory_order_release);
    g_battleRegistered.store(false, std::memory_order_release);
    g_battleCharaVectorAddr.store(0, std::memory_order_release);
    g_battleDeadFrames.store(0, std::memory_order_release);
    g_battleStateSlot.store(0, std::memory_order_release);
    g_lastBattleStateVt.store(0, std::memory_order_release);
    g_battleTickCounter.store(0, std::memory_order_release);
    g_snodeRestoreDeadlineMs.store(0, std::memory_order_release);
    g_battleActive.store(true, std::memory_order_release);
    if (sceneTraceEnabled())
      atfix::log("==== BATTLE_START ms=", GetTickCount64(),
        " gamemode=", reinterpret_cast<void*>(gameMode),
        " helper=", reinterpret_cast<void*>(helper),
        " scene=", reinterpret_cast<void*>(resource), " ====");
    if (battleShadowRestoreEnabled() && g_battleAddrs->casterRestore &&
        gameMode &&
        locateBattleCharaContainer(gameMode, resource, "init")) {
      g_battleContainerFound.store(true, std::memory_order_release);
      registerBattleCharaShadows();
    }
  } else if (g_battleAddrs && callerRva == g_battleAddrs->fieldReentryRet) {
    g_battleActive.store(false, std::memory_order_release);
    // Drop the last observed battle state so arlandInCinematicBattle() cannot
    // stay latched true in the field after a battle that ended in a cinematic
    // state (the tracker only runs while a battle is active).
    g_lastBattleStateVt.store(0, std::memory_order_release);
    // Undo a battle-helper publish so the field renders its own helper again.
    // Unlike restorePublishedHelper, the save is dropped here whether or not it
    // could be written back, and that is deliberate: this is the field scene
    // setup path, so the helper being restored belongs to the scene that is
    // being replaced. Holding it for a later retry would write a destroyed
    // scene's helper into a freshly built one.
    const uintptr_t saved =
      g_savedGlobalHelper.exchange(0, std::memory_order_acq_rel);
    if (saved) {
      if (uintptr_t* slot = globalActiveHelperSlot())
        *slot = saved;
    }
  }
  size_t replayed = 0;
  if (battleShadowRestoreEnabled() && g_battleAddrs &&
      g_battleAddrs->casterRestore &&
      callerRva == g_battleAddrs->battlePublishRet)
    replayed = dispatchBattleCharaShadows(helper, resource);
  if (sceneTraceEnabled()) {
    // The per-frame scene shadow pass reads the active helper from the manager
    // global (+helperSlotOffset) and early-outs when it is null; report it only
    // for scene diagnostics so a normal run does not perform log-only reads.
    const uintptr_t globalMgr = gameBase && g_battleAddrs &&
        g_battleAddrs->managerSlot
      ? *reinterpret_cast<const uintptr_t*>(
          gameBase + g_battleAddrs->managerSlot) : 0;
    const uintptr_t globalActiveHelper = globalMgr
      ? *reinterpret_cast<const uintptr_t*>(
          globalMgr + g_battleAddrs->helperSlotOffset) : 0;
    atfix::log("SHADOW_HELPER_INIT caller_rva=0x", std::hex, callerRva,
      std::dec,
      " global_active_helper=", reinterpret_cast<void*>(globalActiveHelper),
      " helper=", reinterpret_cast<void*>(helper),
      " id=", id,
      " resource=", reinterpret_cast<void*>(resource),
      " config=", reinterpret_cast<void*>(config),
      " slot08=", reinterpret_cast<void*>(
        helper ? *reinterpret_cast<const uintptr_t*>(helper + 0x08) : 0),
      " slot10=", reinterpret_cast<void*>(
        helper ? *reinterpret_cast<const uintptr_t*>(helper + 0x10) : 0),
      " slot18=", reinterpret_cast<void*>(
        helper ? *reinterpret_cast<const uintptr_t*>(helper + 0x18) : 0),
      " slot30=", reinterpret_cast<void*>(
        helper ? *reinterpret_cast<const uintptr_t*>(helper + 0x30) : 0),
      " slot38=", reinterpret_cast<void*>(
        helper ? *reinterpret_cast<const uintptr_t*>(helper + 0x38) : 0),
      " result=", result,
      " replayed=", replayed);
  }
  return result;
}

uintptr_t tracedBattleActorInit(uintptr_t actor, uintptr_t scene) {
  const bool alreadyInitialized = actor && g_battleAddrs &&
    *reinterpret_cast<const uint8_t*>(
      actor + g_battleAddrs->initFlagOffset) != 0;
  const uintptr_t gameMode = actor
    ? *reinterpret_cast<const uintptr_t*>(actor + 0x10) : 0;
  const uintptr_t character = actor
    ? *reinterpret_cast<const uintptr_t*>(actor + 0x18) : 0;
  const uintptr_t helper = gameMode && g_battleAddrs
    ? gameMode + g_battleAddrs->helperEmbedOffset : 0;
  const uintptr_t contextBefore = helper
    ? *reinterpret_cast<const uintptr_t*>(helper + 0x18) : 0;
  const uintptr_t result = originalBattleActorInit(actor, scene);
  bool deferred = false;
  if (battleShadowRestoreEnabled() && !alreadyInitialized && result &&
      helper && character && !contextBefore) {
    std::lock_guard<std::mutex> lock(pendingBattleShadowMutex);
    const auto duplicate = std::find_if(
      pendingBattleShadows.begin(), pendingBattleShadows.end(),
      [&](const PendingBattleShadow& entry) {
        return entry.helper == helper && entry.character == character;
      });
    if (duplicate == pendingBattleShadows.end()) {
      pendingBattleShadows.push_back({helper, scene, character});
      deferred = true;
    }
  }
  if (sceneTraceEnabled())
    atfix::log("BATTLE_ACTOR_INIT actor=", reinterpret_cast<void*>(actor),
      " scene=", reinterpret_cast<void*>(scene),
      " helper=", reinterpret_cast<void*>(helper),
      " character=", reinterpret_cast<void*>(character),
      " already_initialized=", alreadyInitialized,
      " context_before=", reinterpret_cast<void*>(contextBefore),
      " result=", result,
      " deferred=", deferred);
  return result;
}

size_t shadowLayerCount(uintptr_t helper, size_t offset) {
  const auto* vector = reinterpret_cast<const uintptr_t*>(helper + offset);
  const uintptr_t begin = vector[0];
  const uintptr_t end = vector[1];
  if (!begin || end < begin || (end - begin) % sizeof(uintptr_t))
    return 0;
  return size_t((end - begin) / sizeof(uintptr_t));
}

// ==== D: hook installers + installBattleShadowRestore ====
// Battle-shadow hook RVAs per executable build. The multilingual values were
// homologue-matched from the English build; every prologue below is
// byte-identical across the two builds, so the shared expected arrays verify
// both.
struct RoronaShadowHookRvas {
  uintptr_t character, helperInit, battleActorInit, partyCtor, monsterCtor;
};

constexpr RoronaShadowHookRvas kShadowHooksEn = {
  0x1631a0, 0x1611f0, 0x1072a0, 0x110030, 0x10f5d0,
};
constexpr RoronaShadowHookRvas kShadowHooksMulti = {
  0x16b6a0, 0x1696f0, 0x10f3a0, 0x118130, 0x1176d0,
};

bool installRoronaBattleShadowRestore(BYTE* base, const Game& game) {
  if (!battleShadowRestoreEnabled() || game.atlasVariant != AtlasRorona)
    return false;
  const RoronaShadowHookRvas& rvas = game.exeBuild == BuildMultilingual
    ? kShadowHooksMulti : kShadowHooksEn;
  auto* character = base + rvas.character;
  auto* helperInit = base + rvas.helperInit;
  auto* battleActorInit = base + rvas.battleActorInit;
  auto* partyCtor = base + rvas.partyCtor;
  auto* monsterCtor = base + rvas.monsterCtor;
  const std::array<BYTE, 16> characterExpected = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x57, 0x48, 0x83,
    0xec, 0x20, 0x49, 0x8b, 0xd8, 0x48, 0x8b, 0xf9,
  };
  const std::array<BYTE, 16> helperInitExpected = {
    0x48, 0x8b, 0xc4, 0x55, 0x56, 0x57, 0x48, 0x81,
    0xec, 0x00, 0x01, 0x00, 0x00, 0x48, 0xc7, 0x44,
  };
  const std::array<BYTE, 16> battleActorInitExpected = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x30, 0x48,
  };
  const std::array<BYTE, 16> partyCtorExpected = {
    0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe,
  };
  const std::array<BYTE, 16> monsterCtorExpected = {
    0x40, 0x53, 0x56, 0x57, 0x48, 0x83, 0xec, 0x60,
    0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff,
  };
  if (!matches(character, characterExpected) ||
      !matches(helperInit, helperInitExpected) ||
      !matches(battleActorInit, battleActorInitExpected) ||
      !matches(partyCtor, partyCtorExpected) ||
      !matches(monsterCtor, monsterCtorExpected))
    return false;
  originalShadowCharacterBuild =
    reinterpret_cast<ShadowCharacterBuildProc>(character);
  if (!installMinHookDetour(helperInit,
      reinterpret_cast<void*>(&tracedShadowHelperInit),
      reinterpret_cast<void**>(&originalShadowHelperInit)))
    return false;
  if (!installMinHookDetour(battleActorInit,
      reinterpret_cast<void*>(&tracedBattleActorInit),
      reinterpret_cast<void**>(&originalBattleActorInit)))
    return false;
  if (!installMinHookDetour(partyCtor,
      reinterpret_cast<void*>(&tracedBtlCharaPartyCtor),
      reinterpret_cast<void**>(&originalBtlCharaPartyCtor)))
    return false;
  return installMinHookDetour(monsterCtor,
      reinterpret_cast<void*>(&tracedBtlCharaMonsterCtor),
      reinterpret_cast<void**>(&originalBtlCharaMonsterCtor));
}

// Meruru (A13V) battle wiring. Unlike Rorona, Meruru's engine revision already
// registers battle casters natively (its per-character model-build path calls
// ShadowCharacterBuild into the gameMode+0x68 helper — a call site absent from
// Rorona), so the overview has shadows without any caster restoration and none
// of the v0.3 hook set is installed (casterRestore=false in the address pack).
// What Meruru lacks — identically to Rorona — is shadows during the attack
// cut-in, where the designed scene-dim closes the ground receiver's shadow-
// reception gate. The fix is the game-agnostic dim/gate hold in sync_fix.cpp,
// which fires on arlandInCinematicBattle(); all it needs from this side is
// battle-state tracking. That takes exactly one hook: the ShadowHelperInit
// observer, whose battle/field call sites (battlePublishRet/fieldReentryRet)
// flip g_battleActive and seed g_battleGameMode for the per-frame state scan.
// The hooked prologue is byte-identical across Rorona EN, both Meruru builds,
// and both Totori builds; the RVAs are per-build (Meruru EN 0x17b540, Meruru
// multilingual 0x168b20, Totori EN 0x1a8930, Totori multilingual 0x3c4e40),
// each confirmed as the target of the two known call sites. Totori reuses the identical mechanism — its
// fighting shadows are natively healthy (2026-07-23 probe), so like Meruru it
// only needs the state tracking for the cut-in patches.
// Tactical-scene hooks (see the caster-clear block above). Installed only when
// a cut-in hold is enabled — they exist to protect it from stray shadows.
// hideAll's prologue is byte-identical across all five battle-capable builds;
// showAll differs per engine generation.
bool installTacticalSceneHooks(BYTE* base, const Game& game) {
  if (!battleShadowRestoreEnabled() || !g_battleAddrs ||
      !g_battleAddrs->hideAllRva || !g_battleAddrs->showAllRva)
    return false;
  if (!atfix::featureEnabled(atfix::Feature::CutInShadows) &&
      !atfix::featureEnabled(atfix::Feature::CutInDimHold))
    return false;
  auto* hideAll = base + g_battleAddrs->hideAllRva;
  auto* showAll = base + g_battleAddrs->showAllRva;
  const std::array<BYTE, 16> hideAllExpected = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x41, 0xb9,
    0x03, 0x00, 0x00, 0x00, 0x0f, 0x28, 0xd1, 0x33,
  };
  const std::array<BYTE, 16> showAllRoronaExpected = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x41, 0xb8,
    0x03, 0x00, 0x00, 0x00, 0x0f, 0x57, 0xc9, 0x48,
  };
  // Meruru/Totori showAll prologues end in a RIP displacement (stack-cookie
  // load), so the verified window stops before it.
  const std::array<BYTE, 9> showAllMeruruExpected = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b, 0x05,
  };
  const std::array<BYTE, 14> showAllTotoriExpected = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x60, 0x0f, 0x29,
    0x74, 0x24, 0x50, 0x48, 0x8b, 0x05,
  };
  bool showAllOk = false;
  if (game.atlasVariant == AtlasRorona)
    showAllOk = matches(showAll, showAllRoronaExpected);
  else if (game.atlasVariant == AtlasLaterArland)
    showAllOk = matches(showAll, showAllMeruruExpected);
  else if (game.atlasVariant == AtlasTotori)
    showAllOk = matches(showAll, showAllTotoriExpected);
  if (!matches(hideAll, hideAllExpected) || !showAllOk)
    return false;
  if (!installMinHookDetour(hideAll,
      reinterpret_cast<void*>(&tracedTacticalHideAll),
      reinterpret_cast<void**>(&originalTacticalHideAll)))
    return false;
  if (!installMinHookDetour(showAll,
      reinterpret_cast<void*>(&tracedTacticalShowAll),
      reinterpret_cast<void**>(&originalTacticalShowAll)))
    return false;
  g_tacticalHooksActive.store(true, std::memory_order_release);
  // Per-actor deferred-hide front-run: fixes the mid-cut-in stray shadow of a
  // battler hidden during the close-up (see tracedDeferredHideArm — force-
  // expiry, engine-native, zero manual node writes). Validated in Rorona,
  // Meruru, and both Totori builds; ARLAND_CUTIN_ACTOR_CLEAR=0 remains the kill
  // switch for an A/B. On Totori a hidden battler's shadow now goes at fade
  // start rather than fade end, removing the pop where the shadow held full
  // strength while the character faded.
  static const bool actorClearEnabled = [] {
    const char* value = std::getenv("ARLAND_CUTIN_ACTOR_CLEAR");
    return !value || value[0] != '0';
  }();
  if (actorClearEnabled && g_battleAddrs->deferredHideArmRva &&
      g_battleAddrs->modelVisibilityOffset &&
      g_battleAddrs->modelFadePendingOffset &&
      g_battleAddrs->modelFadeDurationOffset) {
    auto* arm = base + g_battleAddrs->deferredHideArmRva;
    // The setter is the same 30-byte leaf in every build, differing only in the
    // two Model offsets it encodes as disp32 fields:
    //   cmp byte [rcx+visibility], dl / je / movss [rcx+duration], xmm2
    // So build the verification window from this build's offsets rather than
    // hardcoding one build's bytes. That makes the check self-validating: a
    // wrong offset fails the byte match and leaves the hook uninstalled,
    // instead of installing and then writing to the wrong field in a live game.
    std::array<BYTE, 16> armExpected = {
      0x38, 0x91, 0x00, 0x00, 0x00, 0x00, 0x74, 0x15,
      0xf3, 0x0f, 0x11, 0x91, 0x00, 0x00, 0x00, 0x00,
    };
    const auto encodeDisp32 = [&armExpected](size_t at, uintptr_t offset) {
      const uint32_t disp = static_cast<uint32_t>(offset);
      std::memcpy(armExpected.data() + at, &disp, sizeof(disp));
    };
    encodeDisp32(2, g_battleAddrs->modelVisibilityOffset);
    encodeDisp32(12, g_battleAddrs->modelFadeDurationOffset);
    if (matches(arm, armExpected)) {
      const bool armed = installMinHookDetour(arm,
        reinterpret_cast<void*>(&tracedDeferredHideArm),
        reinterpret_cast<void**>(&originalDeferredHideArm));
      g_deferredHideArmActive.store(armed, std::memory_order_release);
      if (sceneTraceEnabled())
        atfix::log("Deferred-hide arm hook installed=", armed);
    }
  }
  return true;
}

bool installMeruruBattleStateHook(BYTE* base, const Game& game) {
  if (!battleShadowRestoreEnabled())
    return false;
  uintptr_t helperInitRva = 0;
  if (game.atlasVariant == AtlasLaterArland)
    helperInitRva = game.exeBuild == BuildMultilingual ? 0x168b20 : 0x17b540;
  else if (game.atlasVariant == AtlasTotori)
    helperInitRva = game.exeBuild == BuildMultilingual ? 0x3c4e40 : 0x1a8930;
  if (!helperInitRva)
    return false;
  auto* helperInit = base + helperInitRva;
  const std::array<BYTE, 16> helperInitExpected = {
    0x48, 0x8b, 0xc4, 0x55, 0x56, 0x57, 0x48, 0x81,
    0xec, 0x00, 0x01, 0x00, 0x00, 0x48, 0xc7, 0x44,
  };
  if (!matches(helperInit, helperInitExpected))
    return false;
  return installMinHookDetour(helperInit,
    reinterpret_cast<void*>(&tracedShadowHelperInit),
    reinterpret_cast<void**>(&originalShadowHelperInit));
}

// ---- the battle gate: GameModeBattle construction and destruction ----------
//
// The engine allocates a GameModeBattle for each battle and deletes it when the
// battle's own end states report finished, so its constructor and destructor
// are exact, once-per-battle edges. They replace what used to be inferred: a
// ShadowHelperInit observer that never fired when a battle returned to a field
// map that was still loaded, plus a watchdog that declared the battle over once
// the game mode stopped looking alive for twenty consecutive frames.
//
// The watchdog is left in place as a fallback. It costs nothing while this
// works, because it only runs when the state it clears is still set.
// ARLAND_BATTLE_MODE_GATE=0 stands this down to compare against the old
// behaviour.

// Defined with the rest of the state tracking below.
void restorePublishedHelper(const char* reason);

using BattleModeCtorProc = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t,
                                         uintptr_t);
using BattleModeDtorProc = uintptr_t (*)(uintptr_t);
BattleModeCtorProc originalBattleModeCtor = nullptr;
BattleModeDtorProc originalBattleModeDtor = nullptr;

bool battleModeGateEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_BATTLE_MODE_GATE");
    return !value || value[0] != '0';
  }();
  return enabled;
}

uintptr_t tracedBattleModeCtor(uintptr_t self, uintptr_t a, uintptr_t b,
                               uintptr_t c) {
  const uintptr_t result = originalBattleModeCtor(self, a, b, c);
  // After the original: the mode is fully built here, which is what the rest of
  // the battle code expects to be able to read.
  g_battleGameMode.store(self, std::memory_order_release);
  g_battleSeenLiveMode.store(self, std::memory_order_release);
  g_battleDeadFrames.store(0, std::memory_order_release);
  g_battleActive.store(true, std::memory_order_release);
  if (sceneTraceEnabled())
    atfix::log("==== BATTLE_BEGIN ms=", GetTickCount64(),
      " mode=", reinterpret_cast<void*>(self), " (mode ctor) ====");
  return result;
}

uintptr_t tracedBattleModeDtor(uintptr_t self) {
  // Before the original, while the object is still coherent and before anything
  // it owns is freed. Everything the mod holds about this battle is dropped
  // here, including the game-mode pointer itself: the watchdog dereferences it
  // to test liveness, and one instruction after the original runs it is a
  // pointer into freed memory.
  const uintptr_t tracked = g_battleGameMode.load(std::memory_order_acquire);
  if (!tracked || tracked == self) {
    restorePublishedHelper("battle_mode_dtor");
    g_battleActive.store(false, std::memory_order_release);
    g_battleGameMode.store(0, std::memory_order_release);
    g_lastBattleStateVt.store(0, std::memory_order_release);
    g_battleStateSlot.store(0, std::memory_order_release);
    g_battleSeenLiveMode.store(0, std::memory_order_release);
    g_battleContainerFound.store(false, std::memory_order_release);
    g_battleRegistered.store(false, std::memory_order_release);
    g_battleDeadFrames.store(0, std::memory_order_release);
    g_snodeRestoreDeadlineMs.store(0, std::memory_order_release);
    clearPendingBattleShadows();
    if (sceneTraceEnabled())
      atfix::log("==== BATTLE_END ms=", GetTickCount64(),
        " mode=", reinterpret_cast<void*>(self), " (mode dtor) ====");
  } else {
    // Two live battle modes should be impossible. Say so rather than quietly
    // tearing down state that belongs to a different one.
    static std::atomic<bool> reported{false};
    if (sceneTraceEnabled() ||
        !reported.exchange(true, std::memory_order_relaxed))
      atfix::log("BATTLE_MODE_DTOR for an untracked mode=",
        reinterpret_cast<void*>(self), " tracked=",
        reinterpret_cast<void*>(tracked), "; leaving state alone");
  }
  return originalBattleModeDtor(self);
}

// Both functions start with a stock MSVC frame that matches thousands of others
// in the same binary, so a prologue comparison would prove nothing. What
// identifies them is that each installs the GameModeBattle vtable: find the
// `lea rax, [rip+disp32]` in the prologue and require its target to be that
// vtable. That is the identity claim itself rather than a proxy for it, and it
// re-derives per build from the pack's own numbers.
bool installsBattleModeVtable(BYTE* function, BYTE* base, uintptr_t vtableRva) {
  for (size_t offset = 0; offset + 7 <= 0x60; ++offset) {
    if (function[offset] != 0x48 || function[offset + 1] != 0x8d ||
        function[offset + 2] != 0x05)
      continue;
    int32_t displacement = 0;
    std::memcpy(&displacement, function + offset + 3, sizeof(displacement));
    if (function + offset + 7 + displacement == base + vtableRva)
      return true;
  }
  return false;
}

// ---- the field-tick restore -----------------------------------------------
//
// The global active-helper slot is NOT read per frame: the engine hands that
// pointer out once, at map load, into a tree the scene pass owns and into a
// render-context field. So a battle helper published into it is not something
// the field re-reads and recovers from -- and putting the field's helper back
// at some moment after the battle is a race whose deadline nobody knows.
//
// This makes the restore a state rather than an edge. The field mode's update
// runs only while the field is the active mode, which is exactly when the
// field's own helper belongs in that slot, so every tick simply asserts it.
// There is no battle-end detection, no window, and running it a thousand times
// costs a comparison each.
using FmCoreUpdateProc = void (*)(uintptr_t, float);
FmCoreUpdateProc originalFmCoreUpdate = nullptr;

void tracedFmCoreUpdate(uintptr_t self, float delta) {
  const uintptr_t saved = g_savedGlobalHelper.load(std::memory_order_acquire);
  if (saved) {
    if (uintptr_t* slot = globalActiveHelperSlot()) {
      if (*slot != saved) {
        *slot = saved;
        // Logged for the first few, with the battle flag: if this ever fires
        // while a battle is genuinely in progress it would be taking the battle
        // helper away mid-fight, and the log is what would show that.
        static std::atomic<uint32_t> restores{0};
        const uint32_t seen = restores.fetch_add(1, std::memory_order_relaxed);
        if (seen < 8 && sceneTraceEnabled())
          atfix::log("BATTLE_SHADOW_FIELD_RESTORE helper=",
            reinterpret_cast<void*>(saved), " battle_active=",
            g_battleActive.load(std::memory_order_acquire));
      }
    }
  }
  originalFmCoreUpdate(self, delta);
}

// Identified the same way as the battle mode's pair: the prologue is a stock
// MSVC frame, so what proves this is the field mode's update is that the
// address is what sits in slot 1 of the clsFMCore vtable.
bool installFieldTickRestore(BYTE* base) {
  if (!g_battleAddrs || !g_battleAddrs->fmCoreUpdateRva ||
      !g_battleAddrs->fmCoreVtable || !g_battleAddrs->casterRestore)
    return false;
  // Rorona EN and multilingual compile this function identically. The vtable
  // relationship proves its class/slot identity; the full prologue also guards
  // against a same-layout executable revision changing the target body.
  const std::array<BYTE, 16> fmCoreUpdateExpected = {
    0x48, 0x83, 0xec, 0x38, 0x0f, 0x29, 0x74, 0x24,
    0x20, 0x48, 0x81, 0xc1, 0x00, 0x0b, 0x00, 0x00,
  };
  if (!matches(base + g_battleAddrs->fmCoreUpdateRva,
      fmCoreUpdateExpected)) {
    atfix::log("FIELD_TICK_RESTORE declined: update prologue mismatch");
    return false;
  }
  const uintptr_t slot1 = *reinterpret_cast<uintptr_t*>(
    base + g_battleAddrs->fmCoreVtable + sizeof(uintptr_t));
  if (slot1 != reinterpret_cast<uintptr_t>(base) +
        g_battleAddrs->fmCoreUpdateRva) {
    atfix::log("FIELD_TICK_RESTORE declined: clsFMCore vtable slot 1 is not the"
      " expected update");
    return false;
  }
  return installMinHookDetour(base + g_battleAddrs->fmCoreUpdateRva,
    reinterpret_cast<void*>(&tracedFmCoreUpdate),
    reinterpret_cast<void**>(&originalFmCoreUpdate));
}

// Destructor first: a partial install has to leave the mod inert rather than
// tracking battles it will never see the end of.
bool installBattleModeGate(BYTE* base) {
  if (!battleModeGateEnabled() || !g_battleAddrs ||
      !g_battleAddrs->battleModeCtorRva || !g_battleAddrs->battleModeDtorRva)
    return false;
  auto* ctor = base + g_battleAddrs->battleModeCtorRva;
  auto* dtor = base + g_battleAddrs->battleModeDtorRva;
  if (!installsBattleModeVtable(ctor, base, g_battleAddrs->battleModeVtable) ||
      !installsBattleModeVtable(dtor, base, g_battleAddrs->battleModeVtable)) {
    atfix::log("BATTLE_MODE_GATE declined: the battle game mode's constructor"
      " and destructor do not install the expected vtable");
    return false;
  }
  if (!installMinHookDetour(dtor,
      reinterpret_cast<void*>(&tracedBattleModeDtor),
      reinterpret_cast<void**>(&originalBattleModeDtor)))
    return false;
  return installMinHookDetour(ctor,
    reinterpret_cast<void*>(&tracedBattleModeCtor),
    reinterpret_cast<void**>(&originalBattleModeCtor));
}

// Battle-shadow-restore installation: pick the per-game battle address/state
// tables, then install the caster-registration, cut-in and
// battle-state hooks. Bundled so the menu hook dispatcher has a single battle
// entry point (the battle subsystem otherwise lives in battle_shadow_restore.cpp).
void installBattleShadowRestore(BYTE* base, const Game& game) {
  if (game.atlasVariant == AtlasRorona) {
    g_battleAddrs = game.exeBuild == BuildMultilingual
      ? &kRoronaAddrsMulti : &kRoronaAddrsEn;
    g_battleStates = game.exeBuild == BuildMultilingual
      ? kBattleStatesMulti : kBattleStatesEn;
    g_battleStateCount = game.exeBuild == BuildMultilingual
      ? std::size(kBattleStatesMulti) : std::size(kBattleStatesEn);
  } else if (game.atlasVariant == AtlasLaterArland) {
    g_battleAddrs = game.exeBuild == BuildMultilingual
      ? &kMeruruAddrsMulti : &kMeruruAddrsEn;
    g_battleStates = game.exeBuild == BuildMultilingual
      ? kBattleStatesMeruruMulti : kBattleStatesMeruruEn;
    g_battleStateCount = game.exeBuild == BuildMultilingual
      ? std::size(kBattleStatesMeruruMulti) : std::size(kBattleStatesMeruruEn);
  } else if (game.atlasVariant == AtlasTotori) {
    g_battleAddrs = game.exeBuild == BuildMultilingual
      ? &kTotoriAddrsMulti : &kTotoriAddrsEn;
    g_battleStates = game.exeBuild == BuildMultilingual
      ? kBattleStatesTotoriMulti : kBattleStatesTotoriEn;
    g_battleStateCount = game.exeBuild == BuildMultilingual
      ? std::size(kBattleStatesTotoriMulti) : std::size(kBattleStatesTotoriEn);
  }
  const bool roronaRestoreInstalled =
    installRoronaBattleShadowRestore(base, game);
  const bool battleStateInstalled = installMeruruBattleStateHook(base, game);
  const bool cutinRequested =
    atfix::featureEnabled(atfix::Feature::CutInShadows) ||
    atfix::featureEnabled(atfix::Feature::CutInDimHold);
  bool tacticalInstalled = false;
  if (g_battleAddrs && g_battleAddrs->hideAllRva)
    tacticalInstalled = installTacticalSceneHooks(base, game);
  const bool battleGateInstalled = installBattleModeGate(base);
  bool fieldRestoreInstalled = false;
  if (g_battleAddrs && g_battleAddrs->fmCoreUpdateRva)
    fieldRestoreInstalled = installFieldTickRestore(base);

  const bool rorona = game.atlasVariant == AtlasRorona;
  const bool battleShadowsRequested =
    rorona && atfix::featureEnabled(atfix::Feature::BattleShadows);
  const char* battleShadowsStatus = !rorona ? "game_native"
    : !battleShadowsRequested ? "off"
    : roronaRestoreInstalled ? "active" : "failed";
  const bool stateTrackingInstalled =
    roronaRestoreInstalled || battleStateInstalled;
  const char* cutinStatus = !cutinRequested ? "off"
    : tacticalInstalled ? "active" : "failed";
  const char* stateTrackingStatus = !battleShadowRestoreEnabled() ? "off"
    : stateTrackingInstalled ? "active" : "failed";
  atfix::log("FIXES battle shadows=", battleShadowsStatus,
    " state_tracking=", stateTrackingStatus,
    " cutin_protection=", cutinStatus,
    " battle_gate=", battleGateInstalled ? "active" : "fallback",
    " field_restore=", rorona
      ? (fieldRestoreInstalled ? "active" : "failed") : "not_applicable");
  if (atfix::verboseLogging())
    atfix::log("DIAGNOSTICS battle actor_clear=",
      g_deferredHideArmActive.load(std::memory_order_acquire));
}

// ==== E: state tracking (was global ns) ====

// Log the active scene identity when it changes. The persistent scene manager
// ([0x1410c73c8]) holds the scene container at +0x10; an attack cut-in is
// suspected to swap in a distinct scene/camera object there. Logging on change
// catches a brief cut-in the coarse per-120-frame monitor would miss. Reads
// only fields the game populates; every access is VirtualQuery-guarded.
void sceneIdentityTick() {
  if (!sceneTraceEnabled() || !gameBase || !g_battleAddrs)
    return;
  const uintptr_t managerSlot =
    reinterpret_cast<uintptr_t>(gameBase) + g_battleAddrs->managerSlot;
  if (!readableRange(managerSlot, sizeof(uintptr_t)))
    return;
  const uintptr_t manager = *reinterpret_cast<const uintptr_t*>(managerSlot);
  const uintptr_t helper = readableRange(manager, 0xa00)
    ? *reinterpret_cast<const uintptr_t*>(
        manager + g_battleAddrs->helperSlotOffset) : 0;

  // The active scene manager pointer ([0x1410c73c8]) is swapped per scene; log
  // when it or its active helper changes. A cut-in that installs its own manager
  // will surface here.
  if (manager == g_lastSceneA.load(std::memory_order_acquire) &&
      helper == g_lastSceneHelper.load(std::memory_order_acquire))
    return;
  g_lastSceneA.store(manager, std::memory_order_release);
  g_lastSceneHelper.store(helper, std::memory_order_release);
  atfix::log("SCENE_ID manager=", reinterpret_cast<void*>(manager),
    " active_helper=", reinterpret_cast<void*>(helper),
    " battle_active=", g_battleActive.load(std::memory_order_acquire),
    " battle_helper=", reinterpret_cast<void*>(
      g_battleHelper.load(std::memory_order_acquire)));
}

const char* battleStateName(uintptr_t vtable) {
  if (!gameBase || !vtable || !g_battleStates)
    return nullptr;
  const uintptr_t rva = vtable - reinterpret_cast<uintptr_t>(gameBase);
  for (size_t i = 0; i < g_battleStateCount; i++)
    if (g_battleStates[i].rva == rva)
      return g_battleStates[i].name;
  return nullptr;
}

// The current battle state, read straight out of the engine's own structure.
//
// The state machine is a member of the game mode at battleStateMachineOffset,
// and holds its states as an MSVC std::deque<State*> at +0x10 with the element
// count at +0x30. The active state is the deque's back. This is the same route
// the game's own mode update takes, so it costs five guarded reads and no
// search. Returns 0 when there is no battle state, or when any read fails --
// the caller then falls back to the walk below.
//
// _Map, _Mapsize and _Mysize were each read off the disassembly. _Myoff is the
// standard MSVC placement between the last two; the fallback exists so that
// being wrong about it costs performance rather than the feature.
// Empty and Unusable are different answers and must not be conflated. An empty
// state stack is a normal transient -- the engine's own update tests for it
// before doing anything -- and means "no state right now", so the caller stops.
// Unusable means the layout did not hold and the caller should fall back to the
// search. Returning 0 for both would run the full walk during every transient.
enum class StateLookup { Found, Empty, Unusable };

StateLookup currentBattleStateFromDeque(uintptr_t gameMode, uintptr_t* out) {
  if (out)
    *out = 0;
  if (!gameMode || !g_battleAddrs || !g_battleAddrs->battleStateMachineOffset)
    return StateLookup::Unusable;
  const uintptr_t machine = gameMode + g_battleAddrs->battleStateMachineOffset;
  uint64_t size = 0;
  if (!tryRead(machine + 0x30, size))
    return StateLookup::Unusable;
  if (!size)
    return StateLookup::Empty;
  const uintptr_t deque = machine + 0x10;
  uintptr_t map = 0;
  uint64_t mapSize = 0, offset = 0;
  if (!tryRead(deque + 0x08, map) || !map ||
      !tryRead(deque + 0x10, mapSize) || !mapSize || (mapSize & (mapSize - 1)) ||
      !tryRead(deque + 0x18, offset))
    return StateLookup::Unusable;
  // Two 8-byte elements per 16-byte block, which is what the engine's own
  // indexing does: (off >> 1) & (mapSize - 1) picks the block, off & 1 the slot.
  const uint64_t back = offset + size - 1;
  uintptr_t block = 0;
  if (!tryRead(map + ((back >> 1) & (mapSize - 1)) * 8, block) || !block)
    return StateLookup::Unusable;
  uintptr_t state = 0;
  if (!tryRead(block + (back & 1) * 8, state) || !state)
    return StateLookup::Unusable;
  // Rejects a layout that does not hold at all, because a nonsense index reads
  // a pointer that is not one of the known state vtables. It does NOT catch an
  // off-by-one: every pointer in this deque is a valid state, so landing one
  // slot over passes this check. That is what the cross-check below is for.
  uintptr_t vtable = 0;
  if (!tryRead(state, vtable) || !battleStateName(vtable))
    return StateLookup::Unusable;
  if (out)
    *out = state;
  return StateLookup::Found;
}

// Find the game-mode field currently pointing at a battle-state object, so we
// can re-read it cheaply each frame. Read-only, VirtualQuery-guarded. Fallback
// only: currentBattleStateFromDeque above answers this without searching.
uintptr_t findBattleStateSlot(uintptr_t obj, size_t window, int depth,
                              std::unordered_set<uintptr_t>& seen,
                              size_t& budget) {
  if (!obj || (obj & 7) || budget == 0 || !seen.insert(obj).second ||
      !readableRange(obj, window))
    return 0;
  --budget;
  for (size_t off = 0; off + 8 <= window; off += 8) {
    const uintptr_t ptr = *reinterpret_cast<const uintptr_t*>(obj + off);
    if (ptr && !(ptr & 7) && readableRange(ptr, 8) &&
        battleStateName(*reinterpret_cast<const uintptr_t*>(ptr)))
      return obj + off;
  }
  if (depth > 0)
    for (size_t off = 0; off + 8 <= window; off += 8) {
      const uintptr_t ptr = *reinterpret_cast<const uintptr_t*>(obj + off);
      if (uintptr_t slot =
            findBattleStateSlot(ptr, 0x400, depth - 1, seen, budget))
        return slot;
    }
  return 0;
}

// Publish a newly observed battle-state vtable. Shared by both routes to the
// state object, so they report identically.
void noteBattleStateVtable(uintptr_t vt, uintptr_t stateObj) {
  if (!vt || vt == g_lastBattleStateVt.load(std::memory_order_acquire))
    return;
  g_lastBattleStateVt.store(vt, std::memory_order_release);
  const char* name = battleStateName(vt);
  // The RVA travels with the name, and an unrecognized vtable prints as
  // <unknown> rather than as a null const char*: streaming null sets badbit and
  // would take the rest of the log with it, turning "this state is not in the
  // table" into "logging stopped". The RVA is what identifies the missing entry.
  if (sceneTraceEnabled())
    atfix::log("BATTLE_STATE ms=", GetTickCount64(), " state=",
      name ? name : "<unknown>", " vt_rva=0x", std::hex,
      gameBase ? vt - reinterpret_cast<uintptr_t>(gameBase) : vt, std::dec,
      " obj=", reinterpret_cast<void*>(stateObj));
}

void trackBattleStateTick() {
  if (!battleShadowRestoreEnabled() ||
      !g_battleActive.load(std::memory_order_acquire))
    return;
  const uintptr_t gameMode = g_battleGameMode.load(std::memory_order_acquire);
  // The engine's own route first: five guarded reads, no search. Everything
  // below is the fallback for a build whose layout does not match.
  uintptr_t stateObj = 0;
  const StateLookup lookup = currentBattleStateFromDeque(gameMode, &stateObj);
  if (lookup == StateLookup::Empty)
    return;   // no state right now; not a reason to search
  if (lookup == StateLookup::Found) {
    uintptr_t vt = 0;
    if (!tryRead(stateObj, vt))
      return;
    noteBattleStateVtable(vt, stateObj);
    return;
  }
  uintptr_t slot = g_battleStateSlot.load(std::memory_order_acquire);
  if (slot) {
    // Re-validate the cached slot every frame: on a battle->field return the
    // battle game-mode is freed while g_battleActive can still read true (the
    // race the battleShadowFrameTick watchdog documents), leaving this slot
    // pointing into freed memory. tryRead guards BOTH pointer levels -- the slot
    // and the state object it holds -- so the staleness check itself cannot fault
    // on the freed/garbage state object.
    uintptr_t stateObj = 0, vt = 0;
    const bool valid = tryRead(slot, stateObj) && stateObj &&
      tryRead(stateObj, vt) && battleStateName(vt);
    if (!valid) {
      slot = 0;
      g_battleStateSlot.store(0, std::memory_order_release);
    }
  }
  if (!slot) {
    if (!gameMode)
      return;
    std::unordered_set<uintptr_t> seen;
    size_t budget = 2000;
    slot = findBattleStateSlot(gameMode, 0x1000, 2, seen, budget);
    if (!slot)
      return;
    g_battleStateSlot.store(slot, std::memory_order_release);
  }
  uintptr_t fallbackState = 0, vt = 0;
  if (!tryRead(slot, fallbackState) || !fallbackState ||
      !tryRead(fallbackState, vt))
    return;
  noteBattleStateVtable(vt, fallbackState);
}

// The current battle state name, or nullptr outside battle.
const char* currentBattleState() {
  return battleStateName(g_lastBattleStateVt.load(std::memory_order_acquire));
}

// The cinematic states in which the Event system drives characters — the attack
// cut-in (WaitAction / skill states) and the victory sequence. During these we
// re-scan for character/model objects and register any not yet casting.
bool isCinematicState(const char* name) {
  if (!name)
    return false;
  static const char* const kNames[] = {
    "WaitAction", "HelpSkillBefore", "HelpSkillAfter", "ReactionSkillBefore",
    "Reaction", "ResultStart", "ResultCountExp", "ResultDropItem",
    "ResultLevelUp", "DeadBoss", "AfterBattle",
    // Totori's renamed result chain (its other cinematic states share the
    // Rorona/Meruru names above).
    "Result", "AddPay", "DropItem", "LvUp",
  };
  for (const char* n : kNames)
    if (std::strcmp(name, n) == 0)
      return true;
  return false;
}

// The mid-battle ACTION cut-ins only — the subset of cinematic states in which
// a non-focus battler can be hidden and leave a stray shadow. Deliberately
// EXCLUDES the result/victory/teardown states (ResultStart..LvUp, DeadBoss,
// AfterBattle): during the battle→field transition those states overlap with
// the field beginning to arm its own model hides, and force-expiring those
// would abruptly hide field geometry (observed as a ~1 s black screen on the
// gathering-area return). The force-expiry stray fix gates on this, not on the
// broader isCinematicState.
bool isActionCutinState(const char* name) {
  if (!name)
    return false;
  static const char* const kNames[] = {
    "WaitAction", "HelpSkillBefore", "HelpSkillAfter",
    "ReactionSkillBefore", "Reaction",
  };
  for (const char* n : kNames)
    if (std::strcmp(name, n) == 0)
      return true;
  return false;
}

bool inActionCutin() {
  return isActionCutinState(currentBattleState());
}

// Exposed to the D3D layer (sync_fix) so draws can be tagged cut-in vs overview.

// ==== F: arland* exports (was atfix block) ====
bool arlandInCinematicBattle() {
  return isCinematicState(currentBattleState());
}

bool arlandCutinCasterClearActive() {
  // "Can the held-open gate expose a stale caster?" Two ways it cannot: the
  // per-actor front-run clears flags at fade START (all three games), or the
  // tactical clear has taken every caster down for the cut-in and the delayed
  // restore refuses to re-arm before the cinematic ends (all three games).
  // Either way the dim cover is unnecessary and the hold engages immediately.
  return g_deferredHideArmActive.load(std::memory_order_acquire) ||
         g_tacticalHooksActive.load(std::memory_order_acquire);
}

uint32_t arlandSceneGeneration() {
  return g_sceneGeneration.load(std::memory_order_acquire);
}

// Current battle state name for D3D-side logging, or null outside battle.
const char* arlandBattleStateName() {
  return currentBattleState();
}

// Called by the D3D layer when the 1024x1024 battle shadow map is cleared.
void arlandCutinShadowMapCleared() {
  // Sample the battle state here, before this frame's caster draws, not only at
  // Present. The cut-in gate hold and dim hold are decided during the shadow
  // pass, so a state read that happens at Present is a frame behind: leaving a
  // cut-in, the hold stayed applied for one more frame and the shadow outlived
  // the character model. Present still ticks (frames with no shadow pass, and
  // the field-return watchdog); this is idempotent, so whichever runs first
  // wins and the other returns on the unchanged vtable.
  trackBattleStateTick();
}

// ==== G: frame tick + battleFrameTick ====

// Is the battle game-mode still a live battle (party vector — gameMode +
// partyVectorOffset, 0x658 Rorona / 0x648 Meruru — still holds BtlChara
// objects)? Used to detect battle exit so we can un-publish the battle helper
// before the field renders through a freed pointer.
bool battleGameModeLive(uintptr_t gameMode) {
  if (!gameMode || !g_battleAddrs)
    return false;
  const uintptr_t vec = gameMode + g_battleAddrs->partyVectorOffset;
  if (!readableRange(vec, 0x10))
    return false;
  const uintptr_t begin = *reinterpret_cast<const uintptr_t*>(vec);
  const uintptr_t end = *reinterpret_cast<const uintptr_t*>(vec + 8);
  if (!begin || end <= begin || (end - begin) > 0x1000 ||
      (end - begin) % sizeof(uintptr_t) || !readableRange(begin, end - begin))
    return false;
  const uintptr_t elem0 = *reinterpret_cast<const uintptr_t*>(begin);
  return readableRange(elem0, sizeof(uintptr_t)) &&
    isBattleCharaVtable(*reinterpret_cast<const uintptr_t*>(elem0));
}

// Restore the field helper we displaced when publishing the battle helper.
void restorePublishedHelper(const char* reason) {
  if (!g_savedGlobalHelper.load(std::memory_order_acquire))
    return;
  // Resolve the slot before taking the value. Taking it first loses the saved
  // field helper when the lookup fails, and losing it also disarms the field
  // tick, which is keyed on this atomic being non-zero. Left set, the tick
  // retries until the slot is reachable. The exchange stays because the battle
  // mode's destructor and the watchdog can both run this, and exactly one of
  // them must perform the restore.
  uintptr_t* slot = globalActiveHelperSlot();
  if (!slot)
    return;
  const uintptr_t saved =
    g_savedGlobalHelper.exchange(0, std::memory_order_acq_rel);
  if (saved) {
    *slot = saved;
    if (sceneTraceEnabled())
      atfix::log("BATTLE_SHADOW_RESTORE reason=", reason,
        " restored=", reinterpret_cast<void*>(saved));
  }
}

// Per-battle-frame work: locate the party vector and register once (actors may
// spawn after helper-init); self-heal the publish when the battle ends so the
// field never renders through a freed battle helper (returning to the field does
// not go through the field-scene-setup path our helper-init hook watches).
void battleShadowFrameTick() {
  if (!battleShadowRestoreEnabled())
    return;
  const uintptr_t gameMode = g_battleGameMode.load(std::memory_order_acquire);

  // Self-healing battle-end watchdog: while battle tracking is active (or a
  // helper publish is outstanding), watch the battle game-mode. When it stops
  // looking live for several consecutive frames, the battle is over — restore
  // any published helper and stand the tracking down. This must run whether or
  // not a helper was published: on Meruru nothing is published, and returning
  // from battle to an already-loaded field never re-runs the field
  // ShadowHelperInit call site, so without this watchdog g_battleActive (and
  // the cinematic-state flag scanned from the freed battle game-mode) stays
  // stuck for the rest of the field visit (observed: no BATTLE_END,
  // BATTLE_MONITOR ticking through field exploration, cinematic=1 on field).
  if (g_savedGlobalHelper.load(std::memory_order_acquire) ||
      g_battleActive.load(std::memory_order_acquire)) {
    if (battleGameModeLive(gameMode)) {
      g_battleDeadFrames.store(0, std::memory_order_release);
      g_battleSeenLiveMode.store(gameMode, std::memory_order_release);
    } else if ((g_savedGlobalHelper.load(std::memory_order_acquire) ||
                (gameMode &&
                 g_battleSeenLiveMode.load(std::memory_order_acquire) ==
                   gameMode)) &&
        g_battleDeadFrames.fetch_add(1, std::memory_order_acq_rel) >= 20) {
      restorePublishedHelper("gamemode_dead");
      if (sceneTraceEnabled())
        atfix::log("==== BATTLE_END ms=", GetTickCount64(),
          " gamemode=", reinterpret_cast<void*>(gameMode), " ====");
      g_battleActive.store(false, std::memory_order_release);
      g_lastBattleStateVt.store(0, std::memory_order_release);
      g_battleStateSlot.store(0, std::memory_order_release);
      g_battleSeenLiveMode.store(0, std::memory_order_release);
      g_battleContainerFound.store(false, std::memory_order_release);
      g_battleRegistered.store(false, std::memory_order_release);
      g_battleDeadFrames.store(0, std::memory_order_release);
      g_snodeRestoreDeadlineMs.store(0, std::memory_order_release);
      clearPendingBattleShadows();
      return;
    }
  }

  if (!g_battleActive.load(std::memory_order_acquire))
    return;
  // Delayed caster restore after the tactical showAll re-clear. The deadline is
  // a floor, not the condition: restoring while a cinematic is still running
  // re-arms casters on actors that are hidden or mid-fade, which is a shadow
  // under an invisible character. Wait for the battle to leave the cinematic
  // states, which is the point at which everyone is on screen again.
  uint64_t restoreDeadline =
    g_snodeRestoreDeadlineMs.load(std::memory_order_acquire);
  if (restoreDeadline && GetTickCount64() >= restoreDeadline) {
    if (arlandInCinematicBattle()) {
      g_snodeRestoreDeadlineMs.compare_exchange_strong(
        restoreDeadline, GetTickCount64() + 100, std::memory_order_acq_rel);
    } else if (g_snodeRestoreDeadlineMs.compare_exchange_strong(
        restoreDeadline, 0, std::memory_order_acq_rel)) {
      restoreBattleSnodeFlags("tactical_restore");
    }
  }
  const uint64_t tick = g_battleTickCounter.fetch_add(
    1, std::memory_order_relaxed);
  const uintptr_t scene = g_battleScene.load(std::memory_order_acquire);

  if (g_battleAddrs && g_battleAddrs->casterRestore &&
      !g_battleContainerFound.load(std::memory_order_acquire) &&
      tick % 30 == 0 && tick / 30 <= 40 && gameMode &&
      locateBattleCharaContainer(gameMode, scene, "frame")) {
    g_battleContainerFound.store(true, std::memory_order_release);
    registerBattleCharaShadows();
  }
}

// Per-frame battle tick, bundled so the Present hook (traceMenuPresent) has a
// single battle entry point. The battle subsystem otherwise lives in
// battle_shadow_restore.cpp.
void battleFrameTick() {
  sceneIdentityTick();
  trackBattleStateTick();
  battleShadowFrameTick();
}

}  // namespace atfix

namespace arland {
using namespace atfix;   // battleShadowRestoreActive reaches atfix state

// True when the recognized executable is a battle-capable build (Rorona:
// caster restore + state tracking; Meruru: state tracking for the cut-in
// gate/dim) with the battle-shadow machinery enabled; the per-frame battle
// ticks then need the Present hook regardless of the frame-atlas-cache
// setting.
bool battleShadowRestoreActive() {
  return supportedGame && g_battleAddrs && battleShadowRestoreEnabled();
}
}  // namespace arland
