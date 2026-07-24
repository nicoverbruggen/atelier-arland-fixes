// SPDX-License-Identifier: MIT
//
// Battle-shadow-restore subsystem, split out of menu_fix.cpp. Restores battle and
// cut-in character shadows: it tracks the battle state machine, detects cinematic
// / cut-in states, registers party and event-driven characters as shadow casters
// via the engine's ShadowCharacterBuild path, and installs the shadow-node trace,
// cut-in-flag and tactical-scene hooks. The menu core drives it through two entry
// points (installBattleShadowRestore, battleFrameTick); shared globals gameBase /
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
#include "config.h"        // arlandConfigBool
#include "game.h"          // featureEnabled, Feature, currentTitle, Title
#include "hook_util.h"     // Game, matches, installDetour, installMinHookDetour
#include "log.h"
#include "mem.h"           // readableRange, tryRead
#include "menu_internal.h" // gameBase, supportedGame, g_battleActive

namespace atfix {
extern Log log;

// Forward declarations for cross-referenced battle functions (mirrors the decls
// that lived at the top of menu_fix.cpp).
bool arlandInCinematicBattle();
const char* currentBattleState();
bool inActionCutin();


// ==== A: battle typedefs ====
using ShadowLayerBuildProc = void (*)(uintptr_t, uintptr_t);
using ShadowNodeFactoryProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t);
using ShadowNodeMappingProc = uintptr_t (*)(uintptr_t, uintptr_t);
using ShadowShaderBuildProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using ShadowGroupBuildProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using ShadowCharacterBuildProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using ShadowHelperInitProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using BattleActorInitProc = uintptr_t (*)(uintptr_t, uintptr_t);
using BtlCharaCtorProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using ShadowScenePassProc = uintptr_t (*)(uintptr_t);

// ==== B: battle originals + globals ====
ShadowLayerBuildProc originalShadowLayerBuild = nullptr;
ShadowNodeFactoryProc originalShadowRenderNodeFactory = nullptr;
ShadowNodeFactoryProc originalShadowSkinNodeFactory = nullptr;
ShadowNodeMappingProc originalShadowRenderNodeMapping = nullptr;
ShadowNodeMappingProc originalShadowSkinNodeMapping = nullptr;
ShadowShaderBuildProc originalShadowShaderBuild = nullptr;
ShadowGroupBuildProc originalShadowGroupBuild = nullptr;
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

std::mutex pendingBattleShadowMutex;
std::vector<PendingBattleShadow> pendingBattleShadows;

// Battle-shadow reconstruction, enabled by default on the recognized Rorona
// executable (ARLAND_BATTLE_SHADOWS=0 disables). BtlChara-family instances are
// collected as they are constructed; once the battle ShadowHelper finishes
// initializing we register each one as a shadow caster, reproducing the
// slot-45 init (RVA 0x1072a0) the battle flow never dispatches on its characters.
std::mutex battleCharaMutex;
std::vector<uintptr_t> battleCharas;
std::unordered_set<uintptr_t> dispatchedBattleCharas;

// Publish experiment: the per-frame scene shadow pass reads the active helper
// from a manager global; battle never installs its own helper there, so the
// pass keeps processing the stale field helper. When enabled we temporarily
// point that global at the live battle helper for the duration of each pass.
ShadowScenePassProc originalShadowScenePass = nullptr;
std::atomic<uintptr_t> g_battleHelper{0};
std::atomic<uintptr_t> g_battleGameMode{0};
std::atomic<uintptr_t> g_battleScene{0};
std::atomic<uintptr_t> g_battleCharaVectorAddr{0};
std::atomic<uintptr_t> g_savedGlobalHelper{0};
std::unordered_set<uintptr_t> g_registeredCharacters;  // guarded by battleCharaMutex
std::atomic<bool> g_battleActive{false};
std::atomic<bool> g_battleContainerFound{false};
std::atomic<bool> g_battleRegistered{false};
std::atomic<uint64_t> g_scenePassCalls{0};
std::atomic<uint64_t> g_battleTickCounter{0};
std::atomic<uint32_t> g_battleDeadFrames{0};
// The battle game-mode pointer most recently observed alive (party vector
// holding BtlCharas). The battle-end watchdog only arms for a game-mode it has
// seen alive, so a slow battle intro (party not yet spawned) cannot trip it.
std::atomic<uintptr_t> g_battleSeenLiveMode{0};
std::atomic<uintptr_t> g_battleStateSlot{0};
std::atomic<uintptr_t> g_lastBattleStateVt{0};
std::atomic<uint32_t> g_cutinRegistered{0};
std::atomic<uintptr_t> g_lastSceneA{0};
std::atomic<uintptr_t> g_lastSceneB{0};
std::atomic<uintptr_t> g_lastSceneHelper{0};
thread_local uint64_t shadowRenderMappings = 0;
thread_local uint64_t shadowSkinMappings = 0;

// ==== C1: gates/traces/tables/regs ====
bool shadowLayerTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_SHADOW_LAYERS");
    return value && value[0] != '0';
  }();
  return enabled;
}

bool shadowConstructorTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_SHADOW_CONSTRUCTORS");
    return value && value[0] != '0';
  }();
  return enabled;
}

bool shadowMappingTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_SHADOW_MAPPING");
    return value && value[0] != '0';
  }();
  return enabled;
}

// Master switch for the whole battle-shadow subsystem (env ARLAND_BATTLE_SHADOWS,
// ini [Battle] BattleShadows, default on). This is deliberately NOT the per-game
// gate: it also enables Meruru's cinematic battle-state detection
// (installMeruruBattleStateHook depends on it), which the cut-in shadow fix needs.
// Per-game ordinary-combat caster restoration is gated separately by
// g_battleAddrs->casterRestore (Rorona only today; Totori is planned, see TODO).
bool battleShadowRestoreEnabled() {
  static const bool enabled = [] {
    if (const char* value = std::getenv("ARLAND_BATTLE_SHADOWS"))
      return value[0] != '0';   // env overrides the ini
    return atfix::arlandConfigBool("Battle", "BattleShadows", true);  // default on
  }();
  return enabled;
}

bool battleShadowPublishEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_BATTLE_SHADOW_PUBLISH");
    return value && value[0] != '0';
  }();
  return enabled;
}

// Which helper the located battle characters are registered into:
// "battle" (default) = the game-mode-local helper (gameMode+0x68), published
// into the global slot for the battle's duration — the validated
// configuration; "global" = register directly into the global active helper.
bool battleShadowTargetsBattleHelper() {
  static const bool battle = [] {
    const char* value = std::getenv("ARLAND_BATTLE_SHADOW_TARGET");
    return !value || std::strcmp(value, "global") != 0;
  }();
  return battle;
}

// Experiment for the attack cut-in: re-register the party's current render node
// every few frames, so if a cut-in swaps [chara+0x18] to a cinematic model that
// new node also becomes a shadow caster.
bool battleShadowSweepEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_BATTLE_SHADOW_SWEEP");
    return value && value[0] != '0';
  }();
  return enabled;
}

bool sceneTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_SCENE_TRACE");
    return value && value[0] != '0';
  }();
  return enabled;
}

// Cut-in probe: during WaitAction the engine clears bit 0x10000 at +0xc0 of
// selected registered shadow nodes (SNODE_SNAP diff, REPORT §30m) — its own
// per-node caster kill-switch for the action camera. This experiment re-sets
// the bit on our registered battle casters at the top of each shadow scene
// pass, so the engine itself rebuilds their caster state; if the flag gates
// the transform bake, the resulting shadows are engine-correct (pose included).
// §33j: runtime tracer for the PSSG shadow-node flag functions — static
// analysis found the +0xC2 clearer (0x553960) and initializer (0x551f40) but
// NO static callers (function-pointer dispatch); log who calls them live.
bool cutinFlagTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_CUTIN_FLAG_TRACE");
    return value && value[0] != '0';
  }();
  return enabled;
}

bool cutinSnodeFlagEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_CUTIN_SNODE_FLAG");
    return value && value[0] != '0';
  }();
  return enabled;
}

size_t shadowLayerCount(uintptr_t helper, size_t offset);

uintptr_t tracedShadowRenderNodeMapping(uintptr_t mapping, uintptr_t node) {
  ++shadowRenderMappings;
  return originalShadowRenderNodeMapping(mapping, node);
}

uintptr_t tracedShadowSkinNodeMapping(uintptr_t mapping, uintptr_t node) {
  ++shadowSkinMappings;
  return originalShadowSkinNodeMapping(mapping, node);
}

uintptr_t tracedShadowShaderBuild(uintptr_t shader, uintptr_t a2,
                                  uintptr_t a3, uintptr_t a4) {
  const uintptr_t caller = reinterpret_cast<uintptr_t>(
    __builtin_return_address(0));
  const uintptr_t callerRva = gameBase && caller >= uintptr_t(gameBase)
    ? caller - uintptr_t(gameBase) : 0;
  size_t nodes = 0;
  if (shader) {
    const uintptr_t begin = *reinterpret_cast<const uintptr_t*>(shader + 0x10);
    const uintptr_t end = *reinterpret_cast<const uintptr_t*>(shader + 0x18);
    if (begin && end >= begin && (end - begin) % sizeof(uintptr_t) == 0)
      nodes = size_t((end - begin) / sizeof(uintptr_t));
  }
  const uint64_t renderBefore = shadowRenderMappings;
  const uint64_t skinBefore = shadowSkinMappings;
  const uintptr_t result = originalShadowShaderBuild(shader, a2, a3, a4);
  if (!shadowMappingTraceEnabled())
    return result;
  atfix::log("SHADOW_MAPPING caller_rva=0x", std::hex, callerRva, std::dec,
    " shader=", reinterpret_cast<void*>(shader),
    " nodes=", nodes,
    " a2=", reinterpret_cast<void*>(a2),
    " a3=", reinterpret_cast<void*>(a3),
    " a4=", a4,
    " render=", shadowRenderMappings - renderBefore,
    " skin=", shadowSkinMappings - skinBefore,
    " result=", result);
  return result;
}

uintptr_t tracedShadowGroupBuild(uintptr_t group, uintptr_t a2,
                                 uintptr_t a3, uintptr_t context) {
  if (!shadowMappingTraceEnabled())
    return originalShadowGroupBuild(group, a2, a3, context);
  void* frames[12] = {};
  const USHORT frameCount = CaptureStackBackTrace(
    0, static_cast<DWORD>(std::size(frames)), frames, nullptr);
  const uintptr_t caller = reinterpret_cast<uintptr_t>(
    __builtin_return_address(0));
  const uintptr_t callerRva = gameBase && caller >= uintptr_t(gameBase)
    ? caller - uintptr_t(gameBase) : 0;
  atfix::log("SHADOW_GROUP caller_rva=0x", std::hex, callerRva, std::dec,
    " group=", reinterpret_cast<void*>(group),
    " a2=", reinterpret_cast<void*>(a2),
    " a3=", reinterpret_cast<void*>(a3),
    " context=", reinterpret_cast<void*>(context),
    " frames=", frameCount);
  for (USHORT i = 0; i < frameCount; ++i) {
    const uintptr_t address = reinterpret_cast<uintptr_t>(frames[i]);
    if (gameBase && address >= uintptr_t(gameBase))
      atfix::log("SHADOW_GROUP_FRAME index=", i, " rva=0x", std::hex,
        address - uintptr_t(gameBase), std::dec);
  }
  return originalShadowGroupBuild(group, a2, a3, context);
}

uintptr_t tracedShadowCharacterBuild(uintptr_t helper, uintptr_t scene,
                                     uintptr_t character, uintptr_t a4) {
  const uintptr_t caller = reinterpret_cast<uintptr_t>(
    __builtin_return_address(0));
  const uintptr_t callerRva = gameBase && caller >= uintptr_t(gameBase)
    ? caller - uintptr_t(gameBase) : 0;
  const size_t before = helper ? shadowLayerCount(helper, 0x48) : 0;
  const uintptr_t result = originalShadowCharacterBuild(
    helper, scene, character, a4);
  const size_t after = helper ? shadowLayerCount(helper, 0x48) : 0;
  if (!shadowMappingTraceEnabled())
    return result;
  atfix::log("SHADOW_CHARACTER caller_rva=0x", std::hex, callerRva,
    std::dec,
    " helper=", reinterpret_cast<void*>(helper),
    " scene=", reinterpret_cast<void*>(scene),
    " character=", reinterpret_cast<void*>(character),
    " registry_before=", before,
    " registry_after=", after);
  return result;
}

// BtlChara-family vtable RVAs (ImageBase 0x140000000). A collected pointer is
// only dereferenced for the opt-in dispatch if it still carries one of these,
// which rejects freed/reused objects left over from an earlier battle.
// Same class order in both builds; multilingual values homologue-matched and
// slot-verified (REPORT §31).
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
  uintptr_t charaVtable;
  uintptr_t charaBaseVtable;
  uintptr_t eventExecBtlChara;
  uintptr_t eventExecChara;
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
  bool casterRestore;          // install the full caster-restoration hook set
};

constexpr BattleBuildAddrs kRoronaAddrsEn = {
  kBtlCharaVtableRvasEn, std::size(kBtlCharaVtableRvasEn),
  0x74e598, 0x74eb70, 0x76e018, 0x74e3f8,
  0x10c73c8, 0xfe6e1, 0x397307,
  0x9d0, 0x68, 0x658, 0x2d0, 0x10c2c0, 0x10c270, 0xc5f80, true,
};
constexpr BattleBuildAddrs kRoronaAddrsMulti = {
  kBtlCharaVtableRvasMulti, std::size(kBtlCharaVtableRvasMulti),
  0x76c138, 0x76c710, 0x78bc48, 0x76bf98,
  0x11044c8, 0x106781, 0x3ac8d7,
  0x9d0, 0x68, 0x658, 0x2d0, 0x1143c0, 0x114370, 0xce020, true,
};
// Meruru: managerSlot/helperSlotOffset read straight from the caster-group
// build's prologue (EN 0x396f80: mov rax,[rip+...]=0xfe0b30; mov r10,[rax+0x960];
// ML 0x394030 -> 0x1040410). battlePublishRet/fieldReentryRet are the two (and
// only) static ShadowHelperInit call sites; the battle one is preceded by
// lea rcx,[r14+0x68] exactly like Rorona's, the field one is followed by the
// group-build call. partyVectorOffset from the BtlCharaMgr embed (gameMode+0x638
// + vector at +0x10; Rorona control run reproduced the known 0x658). Chara /
// EventExec vtables via RTTI.
constexpr BattleBuildAddrs kMeruruAddrsEn = {
  kBtlCharaVtableRvasMeruruEn, std::size(kBtlCharaVtableRvasMeruruEn),
  0x6681e8, 0x667fe8, 0x66ffb8, 0x668048,
  0xfe0b30, 0x119a47, 0x392875,
  0x960, 0x68, 0x648, 0x2c0, 0x1369b0, 0x136940, 0x102cd0, false,
};
constexpr BattleBuildAddrs kMeruruAddrsMulti = {
  kBtlCharaVtableRvasMeruruMulti, std::size(kBtlCharaVtableRvasMeruruMulti),
  0x664268, 0x664068, 0x66bfa8, 0x6640c8,
  0x1040410, 0x106e97, 0x38f925,
  0x960, 0x68, 0x648, 0x2c0, 0x124080, 0x124010, 0xf0070, false,
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
// (0x1a8930) call sites. BtlChara/Chara/EventExec vtables via RTTI.
const uintptr_t kBtlCharaVtableRvasTotoriEn[] = {
  0x6dbcf8,  // BtlChara
  0x6dbe88,  // BtlCharaEffect
  0x6dbfd8,  // BtlCharaDummy
  0x6dc128,  // BtlCharaSynchro
  0x6dc3c8,  // BtlCharaMonster
  0x6dc518,  // BtlCharaParty
  0x6dc278,  // BtlCharaRemoteWeapon
};
constexpr BattleBuildAddrs kTotoriAddrsEn = {
  kBtlCharaVtableRvasTotoriEn, std::size(kBtlCharaVtableRvasTotoriEn),
  0x6d4350, 0x6d4240, 0x6dbc90, 0x6d4288,
  0, 0x1512f0, 0x94212,
  0, 0x60, 0x5f8, 0, 0x170cb0, 0x170c30, 0, false,
};

// Null until a battle-capable build is recognized; battle-shadow code paths
// treat that as "feature unavailable".
const BattleBuildAddrs* g_battleAddrs = nullptr;

// Battle state-machine vtables (RVA, ImageBase 0x140000000) → name. The current
// state's Update (vtable slot 1) is what runs each frame; recognizing the state
// object lets us log exactly when the attack cut-in (ExecCommand) and victory
// (ResultStart) are active without needing manual F8 marks. Same state order in
// both builds; multilingual values homologue-matched (REPORT §31).
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
// isCinematicState carries the Totori spellings. Multilingual RVAs not yet
// matched.
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

// Any renderable character/model family object — BtlChara (battle) or the
// general Chara/CharaBase (field/event/cinematic). Returns a class label or
// nullptr. Used to catch a cut-in character the Event system drives outside the
// BtlChara party vector.
const char* charaFamilyName(uintptr_t vtable) {
  if (!gameBase || !vtable || !g_battleAddrs)
    return nullptr;
  if (isBattleCharaVtable(vtable))
    return "BtlChara";
  const uintptr_t rva = vtable - reinterpret_cast<uintptr_t>(gameBase);
  if (rva == g_battleAddrs->charaVtable) return "Chara";
  if (rva == g_battleAddrs->charaBaseVtable) return "CharaBase";
  return nullptr;
}

// The Event executors that drive a character during a cinematic. Finding one and
// dumping its referenced pointers should reveal the active render node the cut-in
// draws (which is NOT the character's registered [+0x18] node).
const char* eventExecName(uintptr_t vtable) {
  if (!gameBase || !vtable || !g_battleAddrs)
    return nullptr;
  const uintptr_t rva = vtable - reinterpret_cast<uintptr_t>(gameBase);
  if (rva == g_battleAddrs->eventExecBtlChara) return "EventExecBtlChara";
  if (rva == g_battleAddrs->eventExecChara) return "EventExecChara";
  return nullptr;
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
        atfix::log("BATTLE_SHADOW_DISPATCH chara=",
          reinterpret_cast<void*>(chara), " vtable_rva=0x", std::hex,
          vtable - reinterpret_cast<uintptr_t>(gameBase), std::dec,
          " character=", reinterpret_cast<void*>(character),
          " registry_before=", before, " registry_after=", after);
      }
    }
  }
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
// Read-only: it only logs where the battle character list lives.
uintptr_t chosenBattleHelper();  // forward decl

size_t scanForBattleCharaVectors(uintptr_t obj, size_t window, int depth,
                                 std::unordered_set<uintptr_t>& seen,
                                 size_t& budget, bool registerMode) {
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
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(elem0);
        g_battleCharaVectorAddr.store(obj + off, std::memory_order_release);
        if (!registerMode)
          atfix::log("BATTLE_CONTAINER obj=", reinterpret_cast<void*>(obj),
            " offset=0x", std::hex, off, std::dec,
            " count=", (end - begin) / sizeof(uintptr_t),
            " elem0=", reinterpret_cast<void*>(elem0),
            " vtable_rva=0x", std::hex,
            vt - reinterpret_cast<uintptr_t>(gameBase), std::dec);
        ++found;
        // Register every BtlChara in this vector as a caster (dedup). Catches a
        // cut-in/victory character that lives outside the party vector.
        if (registerMode) {
          const uintptr_t helper = chosenBattleHelper();
          const uintptr_t scene =
            g_battleScene.load(std::memory_order_acquire);
          if (helper && scene && originalShadowCharacterBuild) {
            for (uintptr_t p = begin; p < end; p += sizeof(uintptr_t)) {
              const uintptr_t chara = *reinterpret_cast<const uintptr_t*>(p);
              if (!readableRange(chara, 0x20) || !isBattleCharaVtable(
                    *reinterpret_cast<const uintptr_t*>(chara)))
                continue;
              const uintptr_t character =
                *reinterpret_cast<const uintptr_t*>(chara + 0x18);
              if (!character)
                continue;
              {
                std::lock_guard<std::mutex> lock(battleCharaMutex);
                if (!g_registeredCharacters.insert(character).second)
                  continue;
              }
              originalShadowCharacterBuild(helper, scene, character, 0);
              atfix::log("BATTLE_REACH_REGISTER ms=", GetTickCount64(),
                " obj=", reinterpret_cast<void*>(obj),
                " offset=0x", std::hex, off, std::dec,
                " chara=", reinterpret_cast<void*>(chara),
                " character=", reinterpret_cast<void*>(character),
                " vtable_rva=0x", std::hex,
                *reinterpret_cast<const uintptr_t*>(chara) -
                  reinterpret_cast<uintptr_t>(gameBase), std::dec,
                " registry=", shadowLayerCount(helper, 0x48));
            }
          }
        }
      }
    }
    if (depth > 0) {
      const uintptr_t ptr = *reinterpret_cast<const uintptr_t*>(obj + off);
      found += scanForBattleCharaVectors(
        ptr, 0x400, depth - 1, seen, budget, registerMode);
    }
  }
  return found;
}

// Scan the battle game-mode and scene (two pointer levels) for the party's
// BtlChara vector, logging any hit. Returns the number of vectors found.
size_t locateBattleCharaContainer(uintptr_t gameMode, uintptr_t scene,
                                  const char* phase, bool registerMode) {
  std::unordered_set<uintptr_t> seen;
  size_t budget = 2000;
  size_t found = scanForBattleCharaVectors(
    gameMode, 0x1000, 2, seen, budget, registerMode);
  if (scene)
    found += scanForBattleCharaVectors(
      scene, 0x1000, 2, seen, budget, registerMode);
  if (!registerMode)
    atfix::log("BATTLE_CONTAINER_SCAN phase=", phase,
      " gamemode=", reinterpret_cast<void*>(gameMode),
      " scene=", reinterpret_cast<void*>(scene),
      " found=", found, " objects_scanned=", 2000 - budget);
  return found;
}

// Address of the manager's active-helper slot
// ([manager global]+helperSlotOffset: 0x9d0 Rorona, 0x960 Meruru), or null.
// The manager global RVA is per-game/per-build; on Rorona both values are
// pinned by the scenePass install signature, whose RIP displacement encodes
// exactly this slot; on Meruru both were read from the caster-group build's
// own manager load.
uintptr_t* globalActiveHelperSlot() {
  // managerSlot == 0 marks a game with no global helper slot (Totori).
  if (!gameBase || !g_battleAddrs || !g_battleAddrs->managerSlot)
    return nullptr;
  const uintptr_t manager =
    *reinterpret_cast<uintptr_t*>(gameBase + g_battleAddrs->managerSlot);
  if (!manager)
    return nullptr;
  return reinterpret_cast<uintptr_t*>(manager + g_battleAddrs->helperSlotOffset);
}

// The helper the fix registers casters into: the battle-local one when
// ARLAND_BATTLE_SHADOW_TARGET=battle, else the global active helper.
uintptr_t chosenBattleHelper() {
  if (battleShadowTargetsBattleHelper())
    return g_battleHelper.load(std::memory_order_acquire);
  if (uintptr_t* slot = globalActiveHelperSlot())
    return *slot;
  return 0;
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

  uintptr_t helper = 0;
  const char* which = "global";
  if (battleShadowTargetsBattleHelper()) {
    helper = g_battleHelper.load(std::memory_order_acquire);
    which = "battle";
  } else if (uintptr_t* slot = globalActiveHelperSlot()) {
    helper = *slot;
  }
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
    {
      std::lock_guard<std::mutex> lock(battleCharaMutex);
      g_registeredCharacters.insert(character);
    }
    ++registered;
    atfix::log("BATTLE_SHADOW_REGISTER which=", which,
      " helper=", reinterpret_cast<void*>(helper),
      " chara=", reinterpret_cast<void*>(chara),
      " character=", reinterpret_cast<void*>(character),
      " registry_before=", before, " registry_after=", after);
  }

  // Registering into the battle helper only matters if the renderer traverses
  // it, so publish it into the global slot (saving the field helper to restore
  // on field re-entry). The global-helper target is already the rendered one.
  bool published = false;
  if (battleShadowTargetsBattleHelper()) {
    if (uintptr_t* slot = globalActiveHelperSlot()) {
      g_savedGlobalHelper.store(*slot, std::memory_order_release);
      *slot = helper;
      published = true;
    }
  }
  atfix::log("BATTLE_SHADOW_REGISTER_SUMMARY which=", which,
    " helper=", reinterpret_cast<void*>(helper),
    " scene=", reinterpret_cast<void*>(scene), " registered=", registered,
    " published=", published);
}

// Re-register the party's *current* render nodes. If an attack cut-in swaps a
// character's [chara+0x18] to a cinematic model, that new node is not yet a
// caster; this picks it up. Dedup'd so each distinct node registers once.
void sweepRegisterBattleCharas() {
  if (!originalShadowCharacterBuild)
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
  uintptr_t helper = 0;
  if (battleShadowTargetsBattleHelper())
    helper = g_battleHelper.load(std::memory_order_acquire);
  else if (uintptr_t* slot = globalActiveHelperSlot())
    helper = *slot;
  const uintptr_t scene = g_battleScene.load(std::memory_order_acquire);
  if (!helper || !scene)
    return;

  for (uintptr_t p = begin; p < end; p += sizeof(uintptr_t)) {
    const uintptr_t chara = *reinterpret_cast<const uintptr_t*>(p);
    if (!readableRange(chara, 0x20) ||
        !isBattleCharaVtable(*reinterpret_cast<const uintptr_t*>(chara)))
      continue;
    const uintptr_t character = *reinterpret_cast<const uintptr_t*>(chara + 0x18);
    if (!character)
      continue;
    {
      std::lock_guard<std::mutex> lock(battleCharaMutex);
      if (!g_registeredCharacters.insert(character).second)
        continue;
    }
    originalShadowCharacterBuild(helper, scene, character, 0);
    atfix::log("BATTLE_SHADOW_SWEEP helper=", reinterpret_cast<void*>(helper),
      " chara=", reinterpret_cast<void*>(chara),
      " character=", reinterpret_cast<void*>(character),
      " registry=", shadowLayerCount(helper, 0x48));
  }
}

// Cut-in probe (§30m): during WaitAction the engine clears bit 0x10000 at
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
  // §33p: the disable wrapper (0x552aa0) clears BOTH byte +0xC2 (the caster
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
    if (!snode || !readableRange(snode, 0xc4))
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
  static std::atomic<uint64_t> tick{0};
  static std::atomic<uint32_t> lastRestored{0xffffffff};
  const uint64_t t = tick.fetch_add(1, std::memory_order_relaxed);
  if (restored != lastRestored.exchange(restored) || (t % 300) == 0)
    atfix::log("CUTIN_SNODE_FLAG site=", site, " restored=", restored,
      " stamped=", stamped, " nodes=", nodes,
      " state=", currentBattleState() ? currentBattleState() : "?",
      " tick=", t);
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
    if (!snode || !readableRange(snode, 0xc4))
      continue;
    ++nodes;
    auto* flag = reinterpret_cast<uint32_t*>(snode + 0xc0);
    if (*flag & 0x10000) {
      *flag &= ~0x10000u;
      ++cleared;
    }
  }
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
  // latches on this Model (+0x8f == 1, i.e. the arm did not early-out) during a
  // cinematic state, zero the fade timer at Model+0x90. The engine's own visTick
  // then performs the complete, bookkeeping-correct hide (subtree setVisibility
  // via 0xb9720 plus the +0x8d/+0x8f state the cancel/show path depends on), so
  // there are ZERO manual node writes and the focus actor is untouched (the
  // enumerator never arms it). Cost: the hidden battler pops rather than fading,
  // off-camera and cosmetically negligible.
  // inActionCutin() (NOT arlandInCinematicBattle) — restricted to the mid-
  // battle action cut-ins, excluding the result/victory teardown states where
  // force-expiring would hit the field transition (black-screen risk).
  if ((target & 0xff) == 0 &&
      g_battleActive.load(std::memory_order_acquire) &&
      g_tacticalHooksActive.load(std::memory_order_acquire) &&
      inActionCutin() &&
      readableRange(obj + 0x8f, sizeof(float) + 1) &&
      *reinterpret_cast<const uint8_t*>(obj + 0x8f) == 1) {
    *reinterpret_cast<float*>(obj + 0x90) = 0.0f;
  }
  return result;
}

uintptr_t tracedTacticalHideAll(uintptr_t a, uintptr_t b,
                                uintptr_t c, uintptr_t d) {
  const uintptr_t result = originalTacticalHideAll(a, b, c, d);
  if (g_battleActive.load(std::memory_order_acquire)) {
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

// Detour of the per-frame scene shadow pass (RVA 0x39cfd0). It reads the active
// helper from the manager global and early-outs / processes whatever it finds.
// While a battle is active and publishing is enabled, we swap that global to the
// live battle helper only for the duration of this call, then restore it, so the
// pass renders the battle helper's casters without leaving the global mutated.
uintptr_t tracedShadowScenePass(uintptr_t self) {
  const bool battleActive = g_battleActive.load(std::memory_order_acquire);
  const uintptr_t battleHelper = g_battleHelper.load(std::memory_order_acquire);
  uintptr_t* slot = globalActiveHelperSlot();
  const uintptr_t globalBefore = slot ? *slot : 0;
  const uint64_t call = ++g_scenePassCalls;

  bool swapped = false;
  if (battleShadowPublishEnabled() && battleActive && battleHelper && slot &&
      globalBefore != battleHelper) {
    *slot = battleHelper;
    swapped = true;
  }

  // Cut-in probe (§30m): restore the engine-cleared caster flag before the
  // pass consumes the registry. NOTE: this pass is only invoked for field
  // scenes — the battle-frame call site is the D3D shadow-map clear (see
  // atfix::arlandCutinShadowMapCleared); this one is kept for completeness.
  if (cutinSnodeFlagEnabled() && battleActive)
    restoreBattleSnodeFlags("scene_pass");

  const uintptr_t result = originalShadowScenePass(self);
  if (swapped)
    *slot = globalBefore;

  if (battleActive && (call % 120) == 0)
    atfix::log("SCENE_PASS call=", call, " battle_active=1",
      " global=", reinterpret_cast<void*>(globalBefore),
      " battle_helper=", reinterpret_cast<void*>(battleHelper),
      " swapped=", swapped, " result=", result);
  return result;
}

std::atomic<uint32_t> g_sceneGeneration{0};

uintptr_t tracedShadowHelperInit(uintptr_t helper, uintptr_t id,
                                 uintptr_t resource, uintptr_t config) {
  const uintptr_t caller = reinterpret_cast<uintptr_t>(
    __builtin_return_address(0));
  const uintptr_t callerRva = gameBase && caller >= uintptr_t(gameBase)
    ? caller - uintptr_t(gameBase) : 0;
  const uintptr_t result = originalShadowHelperInit(
    helper, id, resource, config);
  // §33u: any shadow-helper init is a scene (re)build — field re-entry OR
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
    g_cutinRegistered.store(0, std::memory_order_release);
    g_battleStateSlot.store(0, std::memory_order_release);
    g_lastBattleStateVt.store(0, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(battleCharaMutex);
      g_registeredCharacters.clear();
    }
    g_battleTickCounter.store(0, std::memory_order_release);
    g_snodeRestoreDeadlineMs.store(0, std::memory_order_release);
    g_battleActive.store(true, std::memory_order_release);
    atfix::log("==== BATTLE_START ms=", GetTickCount64(),
      " gamemode=", reinterpret_cast<void*>(gameMode),
      " helper=", reinterpret_cast<void*>(helper),
      " scene=", reinterpret_cast<void*>(resource), " ====");
    if (battleShadowRestoreEnabled() && g_battleAddrs->casterRestore &&
        gameMode &&
        locateBattleCharaContainer(gameMode, resource, "init", false)) {
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
  // The per-frame scene shadow pass reads the active helper from the manager
  // global (+helperSlotOffset) and early-outs when it is null; log it here to
  // see whether battle leaves it unset while field publishes it.
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

uintptr_t tracedShadowNodeFactory(const char* kind,
                                  ShadowNodeFactoryProc original,
                                  uintptr_t allocator, uintptr_t source,
                                  uintptr_t context, uintptr_t caller) {
  const uintptr_t result = original(allocator, source, context);
  const uintptr_t callerRva = gameBase && caller >= uintptr_t(gameBase)
    ? caller - uintptr_t(gameBase) : 0;
  atfix::log("SHADOW_CONSTRUCT kind=", kind,
    " caller_rva=0x", std::hex, callerRva, std::dec,
    " allocator=", reinterpret_cast<void*>(allocator),
    " source=", reinterpret_cast<void*>(source),
    " context=", reinterpret_cast<void*>(context),
    " result=", reinterpret_cast<void*>(result));
  return result;
}

uintptr_t tracedShadowRenderNodeFactory(uintptr_t allocator,
                                        uintptr_t source,
                                        uintptr_t context) {
  return tracedShadowNodeFactory("render", originalShadowRenderNodeFactory,
    allocator, source, context, reinterpret_cast<uintptr_t>(
      __builtin_return_address(0)));
}

uintptr_t tracedShadowSkinNodeFactory(uintptr_t allocator,
                                      uintptr_t source,
                                      uintptr_t context) {
  return tracedShadowNodeFactory("skin", originalShadowSkinNodeFactory,
    allocator, source, context, reinterpret_cast<uintptr_t>(
      __builtin_return_address(0)));
}

size_t shadowLayerCount(uintptr_t helper, size_t offset) {
  const auto* vector = reinterpret_cast<const uintptr_t*>(helper + offset);
  const uintptr_t begin = vector[0];
  const uintptr_t end = vector[1];
  if (!begin || end < begin || (end - begin) % sizeof(uintptr_t))
    return 0;
  return size_t((end - begin) / sizeof(uintptr_t));
}

void tracedShadowLayerBuild(uintptr_t helper, uintptr_t scene) {
  const uintptr_t caller = reinterpret_cast<uintptr_t>(
    __builtin_return_address(0));
  const uintptr_t callerRva = gameBase && caller >= uintptr_t(gameBase)
    ? caller - uintptr_t(gameBase) : 0;
  originalShadowLayerBuild(helper, scene);
  if (!helper)
    return;
  atfix::log("SHADOW_LAYERS helper=", reinterpret_cast<void*>(helper),
    " scene=", reinterpret_cast<void*>(scene),
    " caller_rva=0x", std::hex, callerRva, std::dec,
    " registry=", shadowLayerCount(helper, 0x48),
    " base=", shadowLayerCount(helper, 0x60),
    " sky=", shadowLayerCount(helper, 0x78),
    " shadow=", shadowLayerCount(helper, 0x90),
    " transparent=", shadowLayerCount(helper, 0xa8),
    " refraction=", shadowLayerCount(helper, 0xc0),
    " front=", shadowLayerCount(helper, 0xd8),
    " other=", shadowLayerCount(helper, 0xf0));
}

// ==== D: hook installers + installBattleShadowRestore ====
bool installShadowLayerTrace(BYTE* base, const Game& game) {
  if (!shadowLayerTraceEnabled() || game.atlasVariant != AtlasRorona ||
      game.exeBuild != BuildEnglish)
    return false;
  auto* build = base + 0x163250;
  const std::array<BYTE, 16> expected = {
    0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x6c, 0x24,
  };
  if (!matches(build, expected))
    return false;
  return installMinHookDetour(build,
    reinterpret_cast<void*>(&tracedShadowLayerBuild),
    reinterpret_cast<void**>(&originalShadowLayerBuild));
}

bool installShadowConstructorTrace(BYTE* base, const Game& game) {
  if (!shadowConstructorTraceEnabled() || game.atlasVariant != AtlasRorona ||
      game.exeBuild != BuildEnglish)
    return false;
  auto* render = base + 0x556720;
  auto* skin = base + 0x557200;
  const std::array<BYTE, 16> renderExpected = {
    0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x56, 0x57,
    0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24,
  };
  const std::array<BYTE, 16> skinExpected = {
    0x48, 0x89, 0x4c, 0x24, 0x08, 0x55, 0x56, 0x57,
    0x48, 0x83, 0xec, 0x30, 0x48, 0xc7, 0x44, 0x24,
  };
  if (!matches(render, renderExpected) || !matches(skin, skinExpected))
    return false;
  if (!installMinHookDetour(render,
      reinterpret_cast<void*>(&tracedShadowRenderNodeFactory),
      reinterpret_cast<void**>(&originalShadowRenderNodeFactory)))
    return false;
  return installMinHookDetour(skin,
    reinterpret_cast<void*>(&tracedShadowSkinNodeFactory),
    reinterpret_cast<void**>(&originalShadowSkinNodeFactory));
}

// Battle-shadow hook RVAs per executable build. The multilingual values were
// homologue-matched from the English build; every prologue below except
// scenePass is byte-identical across the two builds, so the shared expected
// arrays verify both. scenePass embeds a RIP displacement to the manager
// global, so its expected bytes are per-build and double as a consistency
// check on BattleBuildAddrs::managerSlot.
struct RoronaShadowHookRvas {
  uintptr_t shader, group, character, helperInit, battleActorInit;
  uintptr_t partyCtor, monsterCtor, scenePass, renderMapping, skinMapping;
  std::array<BYTE, 16> scenePassExpected;
};

constexpr RoronaShadowHookRvas kShadowHooksEn = {
  0x15a730, 0x155da0, 0x1631a0, 0x1611f0, 0x1072a0,
  0x110030, 0x10f5d0, 0x39cfd0, 0x555920, 0x555a80,
  { 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x05, 0xed,
    0xa3, 0xd2, 0x00, 0x4c, 0x8b, 0x90, 0xd0, 0x09 },
};
constexpr RoronaShadowHookRvas kShadowHooksMulti = {
  0x162c30, 0x15e2a0, 0x16b6a0, 0x1696f0, 0x10f3a0,
  0x118130, 0x1176d0, 0x3b25a0, 0x56b7f0, 0x56b950,
  { 0x48, 0x83, 0xec, 0x28, 0x48, 0x8b, 0x05, 0x1d,
    0x1f, 0xd5, 0x00, 0x4c, 0x8b, 0x90, 0xd0, 0x09 },
};

using SnodeFlagProc = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t,
                                    uintptr_t);
SnodeFlagProc originalSnodeFlagClear = nullptr;
SnodeFlagProc originalSnodeInit = nullptr;

uintptr_t tracedSnodeFlagClear(uintptr_t a1, uintptr_t a2, uintptr_t a3,
                               uintptr_t a4) {
  const uintptr_t caller =
    reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  const uintptr_t rva = gameBase && caller >= uintptr_t(gameBase)
    ? caller - uintptr_t(gameBase) : 0;
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1, std::memory_order_relaxed) % 20 == 0)
    atfix::log("CUTIN_FLAGCLEAR caller_rva=0x", std::hex, rva, std::dec,
      " node=", reinterpret_cast<void*>(a1),
      " state=", currentBattleState() ? currentBattleState() : "-");
  return originalSnodeFlagClear(a1, a2, a3, a4);
}

uintptr_t tracedSnodeInit(uintptr_t a1, uintptr_t a2, uintptr_t a3,
                          uintptr_t a4) {
  const uintptr_t caller =
    reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  const uintptr_t rva = gameBase && caller >= uintptr_t(gameBase)
    ? caller - uintptr_t(gameBase) : 0;
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1, std::memory_order_relaxed) % 20 == 0)
    atfix::log("CUTIN_FLAGINIT caller_rva=0x", std::hex, rva, std::dec,
      " node=", reinterpret_cast<void*>(a1),
      " state=", currentBattleState() ? currentBattleState() : "-");
  return originalSnodeInit(a1, a2, a3, a4);
}

bool installCutinFlagTrace(BYTE* base, const Game& game) {
  if (!cutinFlagTraceEnabled() || game.atlasVariant != AtlasRorona ||
      game.exeBuild != BuildEnglish)
    return false;
  auto* clear = base + 0x553960;
  auto* init = base + 0x551f40;
  const std::array<BYTE, 15> clearExpected = {
    0x48, 0x83, 0xec, 0x38, 0x80, 0xb9, 0xc0, 0x00,
    0x00, 0x00, 0x00, 0x0f, 0x85, 0x82, 0x00,
  };
  const std::array<BYTE, 16> initExpected = {
    0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0xa8, 0xe8,
  };
  if (!matches(clear, clearExpected) || !matches(init, initExpected))
    return false;
  if (!installMinHookDetour(clear,
      reinterpret_cast<void*>(&tracedSnodeFlagClear),
      reinterpret_cast<void**>(&originalSnodeFlagClear)))
    return false;
  return installMinHookDetour(init,
    reinterpret_cast<void*>(&tracedSnodeInit),
    reinterpret_cast<void**>(&originalSnodeInit));
}

bool installShadowMappingTrace(BYTE* base, const Game& game) {
  if ((!shadowMappingTraceEnabled() && !battleShadowRestoreEnabled()) ||
      game.atlasVariant != AtlasRorona)
    return false;
  const RoronaShadowHookRvas& rvas = game.exeBuild == BuildMultilingual
    ? kShadowHooksMulti : kShadowHooksEn;
  auto* shader = base + rvas.shader;
  auto* group = base + rvas.group;
  auto* character = base + rvas.character;
  auto* helperInit = base + rvas.helperInit;
  auto* battleActorInit = base + rvas.battleActorInit;
  auto* partyCtor = base + rvas.partyCtor;
  auto* monsterCtor = base + rvas.monsterCtor;
  auto* scenePass = base + rvas.scenePass;
  auto* render = base + rvas.renderMapping;
  auto* skin = base + rvas.skinMapping;
  const std::array<BYTE, 16> shaderExpected = {
    0x4c, 0x8b, 0xdc, 0x53, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x48, 0x83, 0xec, 0x58, 0x48, 0x8b,
  };
  const std::array<BYTE, 16> groupExpected = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48,
  };
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
  const std::array<BYTE, 16>& scenePassExpected = rvas.scenePassExpected;
  const std::array<BYTE, 16> renderExpected = {
    0x48, 0x8b, 0xc4, 0x56, 0x57, 0x41, 0x56, 0x48,
    0x81, 0xec, 0xd0, 0x00, 0x00, 0x00, 0x48, 0xc7,
  };
  const std::array<BYTE, 16> skinExpected = {
    0x48, 0x8b, 0xc4, 0x56, 0x57, 0x41, 0x56, 0x48,
    0x81, 0xec, 0xd0, 0x00, 0x00, 0x00, 0x48, 0xc7,
  };
  if (!matches(shader, shaderExpected) || !matches(group, groupExpected) ||
      !matches(character, characterExpected) ||
      !matches(helperInit, helperInitExpected) ||
      !matches(battleActorInit, battleActorInitExpected) ||
      !matches(partyCtor, partyCtorExpected) ||
      !matches(monsterCtor, monsterCtorExpected) ||
      !matches(scenePass, scenePassExpected) ||
      !matches(render, renderExpected) || !matches(skin, skinExpected))
    return false;
  if (!installMinHookDetour(render,
      reinterpret_cast<void*>(&tracedShadowRenderNodeMapping),
      reinterpret_cast<void**>(&originalShadowRenderNodeMapping)))
    return false;
  if (!installMinHookDetour(skin,
      reinterpret_cast<void*>(&tracedShadowSkinNodeMapping),
      reinterpret_cast<void**>(&originalShadowSkinNodeMapping)))
    return false;
  if (!installMinHookDetour(group,
      reinterpret_cast<void*>(&tracedShadowGroupBuild),
      reinterpret_cast<void**>(&originalShadowGroupBuild)))
    return false;
  if (!installMinHookDetour(character,
      reinterpret_cast<void*>(&tracedShadowCharacterBuild),
      reinterpret_cast<void**>(&originalShadowCharacterBuild)))
    return false;
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
  if (!installMinHookDetour(monsterCtor,
      reinterpret_cast<void*>(&tracedBtlCharaMonsterCtor),
      reinterpret_cast<void**>(&originalBtlCharaMonsterCtor)))
    return false;
  if (!installMinHookDetour(scenePass,
      reinterpret_cast<void*>(&tracedShadowScenePass),
      reinterpret_cast<void**>(&originalShadowScenePass)))
    return false;
  return installMinHookDetour(shader,
    reinterpret_cast<void*>(&tracedShadowShaderBuild),
    reinterpret_cast<void**>(&originalShadowShaderBuild));
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
// and Totori EN; the RVAs are per-build (Meruru EN 0x17b540, Meruru
// multilingual 0x168b20, Totori EN 0x1a8930), each confirmed as the target of
// the two known call sites. Totori reuses the identical mechanism — its
// fighting shadows are natively healthy (2026-07-23 probe), so like Meruru it
// only needs the state tracking for the cut-in patches. Totori's multilingual
// helper-init RVA is not yet matched, so that build stays uninstalled.
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
  // expiry, engine-native, zero manual node writes). On by default now that it
  // is validated (Rorona/Meruru); ARLAND_CUTIN_ACTOR_CLEAR=0 is a diagnostic
  // kill switch.
  static const bool actorClearEnabled = [] {
    const char* value = std::getenv("ARLAND_CUTIN_ACTOR_CLEAR");
    return !value || value[0] != '0';
  }();
  if (actorClearEnabled && g_battleAddrs->deferredHideArmRva) {
    auto* arm = base + g_battleAddrs->deferredHideArmRva;
    const std::array<BYTE, 16> armExpected = {
      0x38, 0x91, 0x80, 0x00, 0x00, 0x00, 0x74, 0x15,
      0xf3, 0x0f, 0x11, 0x91, 0x90, 0x00, 0x00, 0x00,
    };
    if (matches(arm, armExpected))
      atfix::log("Deferred-hide arm hook installed=",
        installMinHookDetour(arm,
          reinterpret_cast<void*>(&tracedDeferredHideArm),
          reinterpret_cast<void**>(&originalDeferredHideArm)));
  }
  return true;
}

bool installMeruruBattleStateHook(BYTE* base, const Game& game) {
  if (!battleShadowRestoreEnabled())
    return false;
  uintptr_t helperInitRva = 0;
  if (game.atlasVariant == AtlasLaterArland)
    helperInitRva = game.exeBuild == BuildMultilingual ? 0x168b20 : 0x17b540;
  else if (game.atlasVariant == AtlasTotori &&
           game.exeBuild == BuildEnglish)
    helperInitRva = 0x1a8930;
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

// Battle-shadow-restore installation: pick the per-game battle address/state
// tables, then install the caster-registration, shadow-trace, cut-in and
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
  } else if (game.atlasVariant == AtlasTotori &&
             game.exeBuild == BuildEnglish) {
    g_battleAddrs = &kTotoriAddrsEn;
    g_battleStates = kBattleStatesTotoriEn;
    g_battleStateCount = std::size(kBattleStatesTotoriEn);
  }
  const bool shadowLayerTraceInstalled = installShadowLayerTrace(base, game);
  const bool shadowConstructorTraceInstalled =
    installShadowConstructorTrace(base, game);
  const bool shadowMappingTraceInstalled = installShadowMappingTrace(base, game);
  const bool battleStateInstalled = installMeruruBattleStateHook(base, game);
  if (game.atlasVariant == AtlasLaterArland || game.atlasVariant == AtlasTotori)
    atfix::log("Battle-state hook installed=", battleStateInstalled);
  if (g_battleAddrs && g_battleAddrs->hideAllRva)
    atfix::log("Tactical caster-clear hooks installed=",
      installTacticalSceneHooks(base, game));
  const bool cutinFlagTraceInstalled = installCutinFlagTrace(base, game);
  if (cutinFlagTraceEnabled())
    atfix::log("Cutin flag trace installed=", cutinFlagTraceInstalled);
  atfix::log("Battle-shadow hooks shadow_layers=", shadowLayerTraceInstalled,
    " shadow_constructors=", shadowConstructorTraceInstalled,
    " shadow_mapping=", shadowMappingTraceInstalled);
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

// Find the game-mode field currently pointing at a battle-state object, so we
// can re-read it cheaply each frame. Read-only, VirtualQuery-guarded.
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

void trackBattleStateTick() {
  if (!battleShadowRestoreEnabled() ||
      !g_battleActive.load(std::memory_order_acquire))
    return;
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
    const uintptr_t gameMode = g_battleGameMode.load(std::memory_order_acquire);
    if (!gameMode)
      return;
    std::unordered_set<uintptr_t> seen;
    size_t budget = 2000;
    slot = findBattleStateSlot(gameMode, 0x1000, 2, seen, budget);
    if (!slot)
      return;
    g_battleStateSlot.store(slot, std::memory_order_release);
  }
  uintptr_t stateObj = 0, vt = 0;
  if (!tryRead(slot, stateObj) || !stateObj || !tryRead(stateObj, vt))
    return;
  if (vt == g_lastBattleStateVt.load(std::memory_order_acquire))
    return;
  g_lastBattleStateVt.store(vt, std::memory_order_release);
  const char* name = battleStateName(vt);
  atfix::log("BATTLE_STATE ms=", GetTickCount64(), " state=",
    name, " obj=", reinterpret_cast<void*>(stateObj));

  // On entering the overview (SelectCommand) or the attack cut-in (WaitAction),
  // snapshot the first party character's render node so the two can be diffed to
  // find the flag that gates shadow casting during the cut-in.
  static int snaps = 0;
  const bool target = name && (std::strcmp(name, "SelectCommand") == 0 ||
    std::strcmp(name, "WaitAction") == 0);
  if (!target || snaps >= 4)
    return;
  const uintptr_t gameMode = g_battleGameMode.load(std::memory_order_acquire);
  if (!gameMode || !g_battleAddrs ||
      !readableRange(gameMode + g_battleAddrs->partyVectorOffset, 0x10))
    return;
  const uintptr_t begin = *reinterpret_cast<const uintptr_t*>(
    gameMode + g_battleAddrs->partyVectorOffset);
  const uintptr_t end = *reinterpret_cast<const uintptr_t*>(
    gameMode + g_battleAddrs->partyVectorOffset + 8);
  if (!begin || end <= begin || (end - begin) > 0x200 ||
      !readableRange(begin, end - begin))
    return;
  ++snaps;
  // Dump the shadow caster registry nodes (helper+0x48) — the actual nodes the
  // shadow traversal walks; their per-node visibility gates the shadow draw.
  const uintptr_t helper = gameMode + g_battleAddrs->helperEmbedOffset;
  if (readableRange(helper + 0x48, 0x10)) {
    const uintptr_t rb = *reinterpret_cast<const uintptr_t*>(helper + 0x48);
    const uintptr_t re = *reinterpret_cast<const uintptr_t*>(helper + 0x50);
    if (rb && re > rb && (re - rb) <= 0x200 && readableRange(rb, re - rb)) {
      int ridx = 0;
      for (uintptr_t p = rb; p < re; p += sizeof(uintptr_t), ++ridx) {
        const uintptr_t snode = *reinterpret_cast<const uintptr_t*>(p);
        if (!snode || !readableRange(snode, 0x120))
          continue;
        for (size_t off = 0; off < 0x120; off += 0x20)
          atfix::log("SNODE_SNAP state=", name, " ridx=", ridx,
            " snode=", reinterpret_cast<void*>(snode),
            " off=0x", std::hex, off,
            " ", *reinterpret_cast<const uintptr_t*>(snode + off),
            " ", *reinterpret_cast<const uintptr_t*>(snode + off + 8),
            " ", *reinterpret_cast<const uintptr_t*>(snode + off + 0x10),
            " ", *reinterpret_cast<const uintptr_t*>(snode + off + 0x18),
            std::dec);
      }
    }
  }
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
  return g_tacticalHooksActive.load(std::memory_order_acquire);
}

uint32_t arlandSceneGeneration() {
  return g_sceneGeneration.load(std::memory_order_acquire);
}

// Current battle state name for D3D-side logging, or null outside battle.
const char* arlandBattleStateName() {
  return currentBattleState();
}

// Called by the D3D layer when the 1024x1024 battle shadow map is cleared —
// the only reliable per-battle-frame hook on the render thread (the scene
// shadow pass 0x39cfd0 is field-only). Restores engine-cleared caster flags
// before this frame's caster draws are issued (§30m probe).
void arlandCutinShadowMapCleared() {
  if (!cutinSnodeFlagEnabled() ||
      !g_battleActive.load(std::memory_order_acquire))
    return;
  restoreBattleSnodeFlags("map_clear");
}

// ==== G: cut-in re-register + frame tick + battleFrameTick ====
// Scan an object graph for individually-referenced character/model objects and
// register each one's render node ([obj+0x18]) as a shadow caster (deduped,
// capped). Catches a cut-in/victory character the Event system drives outside
// the party vector. Read-guarded; bounded so the caster registry can't run away.
size_t scanCutinCharas(uintptr_t obj, size_t window, int depth,
                       std::unordered_set<uintptr_t>& seen, size_t& budget,
                       uintptr_t helper, uintptr_t scene, const char* state) {
  if (!obj || (obj & 7) || budget == 0 || !seen.insert(obj).second ||
      !readableRange(obj, window))
    return 0;
  --budget;
  size_t registered = 0;
  for (size_t off = 0; off + 8 <= window; off += 8) {
    const uintptr_t ptr = *reinterpret_cast<const uintptr_t*>(obj + off);
    if (!ptr || (ptr & 7) || !readableRange(ptr, 0x20))
      continue;
    const uintptr_t ptrVt = *reinterpret_cast<const uintptr_t*>(ptr);
    if (const char* ev = eventExecName(ptrVt)) {
      std::lock_guard<std::mutex> lock(battleCharaMutex);
      if (g_registeredCharacters.insert(ptr | 1).second &&  // dedup marker
          readableRange(ptr, 0x80)) {
        for (size_t f = 0; f < 0x80; f += 8) {
          const uintptr_t v = *reinterpret_cast<const uintptr_t*>(ptr + f);
          // Guard 0x20: the class probe reads *v (vtable) and the log line below
          // reads *(v + 0x18), so v must be readable to 0x20, not just 8.
          const char* fc = (v && !(v & 7) && readableRange(v, 0x20))
            ? charaFamilyName(*reinterpret_cast<const uintptr_t*>(v)) : nullptr;
          if (fc)
            atfix::log("EVENT_EXEC ms=", GetTickCount64(), " state=", state,
              " ev=", ev, " obj=", reinterpret_cast<void*>(ptr),
              " field=0x", std::hex, f, std::dec,
              " ref=", reinterpret_cast<void*>(v), " ref_class=", fc,
              " node=", reinterpret_cast<void*>(
                *reinterpret_cast<const uintptr_t*>(v + 0x18)));
        }
      }
    }
    const char* cls = charaFamilyName(ptrVt);
    if (cls) {
      const uintptr_t node = *reinterpret_cast<const uintptr_t*>(ptr + 0x18);
      if (node && readableRange(node, 8)) {
        bool isNew;
        {
          std::lock_guard<std::mutex> lock(battleCharaMutex);
          isNew = g_registeredCharacters.insert(node).second;
        }
        if (isNew && g_cutinRegistered.load(std::memory_order_acquire) < 512) {
          const size_t before = shadowLayerCount(helper, 0x48);
          originalShadowCharacterBuild(helper, scene, node, 0);
          const size_t after = shadowLayerCount(helper, 0x48);
          g_cutinRegistered.fetch_add(1, std::memory_order_acq_rel);
          ++registered;
          atfix::log("BATTLE_CUTIN_REGISTER ms=", GetTickCount64(),
            " state=", state, " class=", cls,
            " obj=", reinterpret_cast<void*>(ptr),
            " node=", reinterpret_cast<void*>(node),
            " registry_before=", before, " registry_after=", after);
        }
      }
    }
    if (depth > 0)
      registered +=
        scanCutinCharas(ptr, 0x400, depth - 1, seen, budget, helper, scene, state);
  }
  return registered;
}

void cutinReregisterTick() {
  if (!battleShadowSweepEnabled() ||
      !g_battleActive.load(std::memory_order_acquire) ||
      !originalShadowCharacterBuild)
    return;
  const char* state = currentBattleState();
  if (!isCinematicState(state))
    return;
  const uintptr_t helper = chosenBattleHelper();
  const uintptr_t scene = g_battleScene.load(std::memory_order_acquire);
  const uintptr_t gameMode = g_battleGameMode.load(std::memory_order_acquire);
  if (!helper || !scene || !gameMode)
    return;
  std::unordered_set<uintptr_t> seen;
  size_t budget = 3000;
  scanCutinCharas(gameMode, 0x1000, 3, seen, budget, helper, scene, state);
  if (scene != gameMode)
    scanCutinCharas(scene, 0x1000, 3, seen, budget, helper, scene, state);
}

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
  const uintptr_t saved =
    g_savedGlobalHelper.exchange(0, std::memory_order_acq_rel);
  if (saved) {
    if (uintptr_t* slot = globalActiveHelperSlot())
      *slot = saved;
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
      return;
    }
  }

  if (!g_battleActive.load(std::memory_order_acquire))
    return;
  // Delayed caster restore after the tactical showAll re-clear.
  uint64_t restoreDeadline =
    g_snodeRestoreDeadlineMs.load(std::memory_order_acquire);
  if (restoreDeadline && GetTickCount64() >= restoreDeadline &&
      g_snodeRestoreDeadlineMs.compare_exchange_strong(
        restoreDeadline, 0, std::memory_order_acq_rel))
    restoreBattleSnodeFlags("tactical_restore");
  const uint64_t tick = g_battleTickCounter.fetch_add(
    1, std::memory_order_relaxed);
  const uintptr_t scene = g_battleScene.load(std::memory_order_acquire);

  if (g_battleAddrs && g_battleAddrs->casterRestore &&
      !g_battleContainerFound.load(std::memory_order_acquire) &&
      tick % 30 == 0 && tick / 30 <= 40 && gameMode &&
      locateBattleCharaContainer(gameMode, scene, "frame", false)) {
    g_battleContainerFound.store(true, std::memory_order_release);
    registerBattleCharaShadows();
  }

  // Sweep mode: continuously scan the whole game-mode graph and register every
  // reachable BtlChara (deduped). Catches a cut-in/victory character that lives
  // outside the party vector and only exists while its scene is on screen.
  if (battleShadowSweepEnabled() && gameMode && tick % 8 == 0)
    locateBattleCharaContainer(gameMode, scene, "sweep", true);

  if (tick % 120 == 0 && tick <= 120 * 200) {
    uintptr_t* slot = globalActiveHelperSlot();
    const uintptr_t global = slot ? *slot : 0;
    const uintptr_t ours = g_battleHelper.load(std::memory_order_acquire);
    atfix::log("BATTLE_MONITOR tick=", tick,
      " global_active_helper=", reinterpret_cast<void*>(global),
      " battle_helper=", reinterpret_cast<void*>(ours),
      " matches=", global == ours && ours != 0);
  }
}

// Per-frame battle tick, bundled so the Present hook (traceMenuPresent) has a
// single battle entry point. The battle subsystem otherwise lives in
// battle_shadow_restore.cpp.
void battleFrameTick() {
  sceneIdentityTick();
  trackBattleStateTick();
  battleShadowFrameTick();
  cutinReregisterTick();
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
