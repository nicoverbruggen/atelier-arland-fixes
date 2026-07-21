// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../vendor/minhook/include/MinHook.h"
#include "log.h"

namespace atfix {
extern Log log;
bool arlandInCinematicBattle();   // defined later in this TU
}

const char* currentBattleState();

namespace {

using PathCheckProc = bool (*)(void*, void*);
using QueueDrainProc = void (*)(void*);
using RenderTextProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using AtlasLockProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using AtlasUnlockProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using NodeInitProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using NodeResourceProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using NodeLayoutProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using RecordProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using LayoutLookupProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using LayoutCreateProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using LayoutApplyProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using LayoutBuildCoreProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t,
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using LayoutEntryInitProc = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t);
using LayoutAcquireProc = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t);
using LayoutApplyCoreProc = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t);
using LayoutVirtualProc = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t);
using LayoutValidateF0Proc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using LayoutRebuildProc = uintptr_t (*)(uintptr_t, uintptr_t);
using GameAllocProc = void* (*)(size_t);
using GameFreeProc = void (*)(void*);
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

struct Game {
  const char* executable;
  DWORD textSize;
  uintptr_t pathCheckRva;
  BYTE pushedRegister;
  uintptr_t queueDrainRva;
  uintptr_t renderTextRva;
  uintptr_t atlasLockRva;
  uintptr_t atlasUnlockRva;
  uint8_t atlasVariant;
  uint8_t exeBuild;
};

enum : uint8_t {
  AtlasNone,
  AtlasRorona,
  AtlasTotori,
  AtlasLaterArland,
};

// Each game ships two executables: the English build (launcher Language=2) and
// the multilingual build (Japanese and both Chinese locales). They are separate
// compiles with distinct RVAs; the multilingual entries below were located by
// static homologue matching against the English build and every hooked prologue
// byte-verified in the multilingual binary (REPORT §31). Hooks whose RVAs are
// only known for the English build stay gated on BuildEnglish.
enum : uint8_t {
  BuildEnglish,
  BuildMultilingual,
};

constexpr Game games[] = {
  { "A11R_x64_Release_en.exe", 0x709a9c, 0x12cc70, 0x57,
    0x08d4b0, 0x5613b0, 0x3eea10, 0x3eea60, AtlasRorona, BuildEnglish },
  { "A11R_x64_Release.exe", 0x72141c, 0x135130, 0x57,
    0x094890, 0x577280, 0x4048e0, 0x404930, AtlasRorona, BuildMultilingual },
  { "A12V_x64_Release_en.exe", 0x67da5c, 0x18b140, 0x56,
    0x038a00, 0x430bf0, 0x4c2080, 0x4c20c0, AtlasTotori, BuildEnglish },
  { "A12V_x64_Release.exe", 0x90e1ec, 0x3a7b20, 0x56,
    0x255020, 0x6ae1f0, 0x73f680, 0x73f6c0, AtlasTotori, BuildMultilingual },
  { "A13V_x64_Release_EN.exe", 0x61ecec, 0x1533c0, 0x57,
    0x0d6210, 0x5115d0, 0x3ea7d0, 0x3ea7f0, AtlasLaterArland, BuildEnglish },
  { "A13V_x64_Release.exe", 0x61ae4c, 0x140d20, 0x57,
    0x0c2e20, 0x510c30, 0x3e9cf0, 0x3e9d10, AtlasLaterArland, BuildMultilingual },
};

PathCheckProc originalPathCheck = nullptr;
QueueDrainProc originalQueueDrain = nullptr;
RenderTextProc originalRenderText = nullptr;
AtlasLockProc originalAtlasLock = nullptr;
AtlasUnlockProc originalAtlasUnlock = nullptr;
NodeInitProc originalNodeInit = nullptr;
NodeResourceProc originalNodeResource = nullptr;
NodeLayoutProc originalNodeLayout = nullptr;
RecordProc originalRecord = nullptr;
LayoutLookupProc originalLayoutLookup = nullptr;
LayoutCreateProc originalLayoutCreate = nullptr;
LayoutApplyProc originalLayoutApply = nullptr;
LayoutBuildCoreProc originalLayoutBuildCore = nullptr;
LayoutEntryInitProc originalLayoutEntryInit = nullptr;
LayoutAcquireProc originalLayoutAcquire = nullptr;
LayoutApplyCoreProc originalLayoutApplyCore = nullptr;
LayoutVirtualProc originalLayoutObjectE8 = nullptr;
LayoutVirtualProc originalLayoutObjectF0 = nullptr;
LayoutVirtualProc originalLayoutObjectC8 = nullptr;
LayoutVirtualProc originalLayoutValidateC8 = nullptr;
LayoutValidateF0Proc originalLayoutValidateF0 = nullptr;
LayoutRebuildProc originalLayoutRebuild = nullptr;
LayoutVirtualProc originalLayoutAllocate = nullptr;
LayoutBuildCoreProc originalLayoutTemplate = nullptr;
GameAllocProc gameAlloc = nullptr;
GameFreeProc gameFree = nullptr;
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
std::atomic<uintptr_t> g_battleStateSlot{0};
std::atomic<uintptr_t> g_lastBattleStateVt{0};
std::atomic<uint32_t> g_cutinRegistered{0};
std::atomic<uintptr_t> g_lastSceneA{0};
std::atomic<uintptr_t> g_lastSceneB{0};
std::atomic<uintptr_t> g_lastSceneHelper{0};
thread_local uint64_t shadowRenderMappings = 0;
thread_local uint64_t shadowSkinMappings = 0;
std::once_flag initialization;
bool supportedGame = false;
BYTE* gameBase = nullptr;
std::mutex cacheMutex;
std::unordered_set<std::string> successfulPaths;

struct AtlasRead {
  uint32_t pitch = 0;
  std::vector<uint8_t> bytes;
};

struct RenderTextKey {
  uintptr_t renderer = 0;
  uintptr_t font = 0;
  uintptr_t atlas = 0;
  uint32_t mode = 0;
  uint8_t variant = 0;
  uint8_t flag = 0;
  std::string text;

  bool operator==(const RenderTextKey& other) const {
    return renderer == other.renderer && font == other.font &&
      atlas == other.atlas && mode == other.mode &&
      variant == other.variant && flag == other.flag && text == other.text;
  }
};

struct RenderTextKeyHash {
  size_t operator()(const RenderTextKey& key) const {
    size_t hash = std::hash<std::string>{}(key.text);
    const std::array<uintptr_t, 6> values = {
      key.renderer, key.font, key.atlas, uintptr_t(key.mode),
      uintptr_t(key.variant), uintptr_t(key.flag),
    };
    for (const uintptr_t value : values)
      hash ^= std::hash<uintptr_t>{}(value) + 0x9e3779b9 +
        (hash << 6) + (hash >> 2);
    return hash;
  }
};

struct RenderTextBitmap {
  int32_t width = 0;
  int32_t height = 0;
  std::array<uint32_t, 4> metrics = {};
  uintptr_t result = 0;
  std::vector<uint8_t> bytes;
};

struct RenderTextOutputSignature {
  int32_t width = 0;
  int32_t height = 0;
  std::array<uint32_t, 4> metrics = {};
  uintptr_t result = 0;
  uint64_t byteCount = 0;
  uint64_t byteHash = 0;

  bool operator==(const RenderTextOutputSignature& other) const {
    return width == other.width && height == other.height &&
      metrics == other.metrics && result == other.result &&
      byteCount == other.byteCount && byteHash == other.byteHash;
  }
};

struct RenderTextKeyTiming {
  int recordType = -1;
  uint64_t calls = 0;
  uint64_t totalNanos = 0;
  uint64_t minimumNanos = UINT64_MAX;
  uint64_t maximumNanos = 0;
};

std::mutex atlasMutex;
std::unordered_map<uintptr_t, AtlasRead> atlasReads;
std::mutex renderBitmapMutex;
std::unordered_map<RenderTextKey, RenderTextBitmap, RenderTextKeyHash>
  renderBitmapCache;
std::unordered_map<RenderTextKey, RenderTextOutputSignature, RenderTextKeyHash>
  renderOutputSignatures;
std::unordered_map<RenderTextKey, RenderTextKeyTiming, RenderTextKeyHash>
  renderKeyTimings;
uint64_t renderOutputRepeatMatches = 0;
uint64_t renderOutputRepeatConflicts = 0;
uint64_t renderOutputInvalidSamples = 0;
std::atomic<uint32_t> atlasDrainDepth = { 0 };
std::atomic<bool> atlasCacheActive = { false };
std::atomic<bool> frameAtlasCacheDefault = { false };
std::atomic<uint64_t> pathCacheHits = { 0 };
std::atomic<uint64_t> pathRealChecks = { 0 };
std::atomic<uint64_t> atlasCacheHits = { 0 };
std::atomic<uint64_t> atlasRealReads = { 0 };
thread_local uint32_t renderTextDepth = 0;
thread_local std::vector<uintptr_t> syntheticAtlasLocks;
thread_local std::vector<uintptr_t> realCandidateAtlasLocks;
thread_local uint32_t nodeInitDepth = 0;
thread_local uint32_t layoutCreateDepth = 0;
thread_local uint32_t layoutApplyDepth = 0;
thread_local std::unordered_set<uint64_t> layoutBuildKeys;
thread_local std::unordered_set<uintptr_t> layoutBuildResults;
thread_local std::unordered_set<uintptr_t> layoutTemplateResults;
thread_local std::unordered_set<uint64_t> renderTextKeys;
thread_local std::unordered_set<uint64_t> renderTextExactKeys;
thread_local std::array<std::unordered_set<uint64_t>, 2>
  renderTextExactKeysByRecordType;
thread_local std::unordered_set<uintptr_t> renderTextPointers;
thread_local std::unordered_set<uintptr_t> renderTextRenderers;
constexpr size_t recordTypeCount = 40;
constexpr size_t recordDepthLimit = 128;
thread_local size_t recordDepth = 0;
thread_local uint32_t type19Depth = 0;
thread_local int activeTextRecordType = -1;
struct ActiveRenderTrace {
  bool active = false;
  uint64_t atlasNanos = 0;
  uint64_t atlasCalls = 0;
  uint64_t atlasCached = 0;
  uint64_t atlasReal = 0;
};
thread_local ActiveRenderTrace activeRenderTrace;
thread_local std::array<uint64_t, recordDepthLimit> recordChildNanos = {};
std::atomic<bool> recordTimingActive = { false };

struct DeepMenuCounters {
  std::atomic<uint64_t> nodeCalls = { 0 };
  std::atomic<uint64_t> nodeNanos = { 0 };
  std::atomic<uint64_t> resourceCalls = { 0 };
  std::atomic<uint64_t> resourceNanos = { 0 };
  std::atomic<uint64_t> layoutCalls = { 0 };
  std::atomic<uint64_t> layoutNanos = { 0 };
  std::atomic<uint64_t> layoutLookupCalls = { 0 };
  std::atomic<uint64_t> layoutLookupNanos = { 0 };
  std::atomic<uint64_t> layoutCreateCalls = { 0 };
  std::atomic<uint64_t> layoutCreateNanos = { 0 };
  std::atomic<uint64_t> layoutApplyCalls = { 0 };
  std::atomic<uint64_t> layoutApplyNanos = { 0 };
  std::atomic<uint64_t> layoutBuildCoreCalls = { 0 };
  std::atomic<uint64_t> layoutBuildCoreNanos = { 0 };
  std::atomic<uint64_t> layoutEntryInitCalls = { 0 };
  std::atomic<uint64_t> layoutEntryInitNanos = { 0 };
  std::atomic<uint64_t> layoutAcquireCalls = { 0 };
  std::atomic<uint64_t> layoutAcquireNanos = { 0 };
  std::atomic<uint64_t> layoutApplyCoreCalls = { 0 };
  std::atomic<uint64_t> layoutApplyCoreNanos = { 0 };
  std::atomic<uint64_t> layoutObjectE8Calls = { 0 };
  std::atomic<uint64_t> layoutObjectE8Nanos = { 0 };
  std::atomic<uint64_t> layoutObjectF0Calls = { 0 };
  std::atomic<uint64_t> layoutObjectF0Nanos = { 0 };
  std::atomic<uint64_t> layoutObjectC8Calls = { 0 };
  std::atomic<uint64_t> layoutObjectC8Nanos = { 0 };
  std::atomic<uint64_t> layoutValidateC8Calls = { 0 };
  std::atomic<uint64_t> layoutValidateC8Nanos = { 0 };
  std::atomic<uint64_t> layoutValidateF0Calls = { 0 };
  std::atomic<uint64_t> layoutValidateF0Nanos = { 0 };
  std::atomic<uint64_t> layoutRebuildCalls = { 0 };
  std::atomic<uint64_t> layoutRebuildNanos = { 0 };
  std::atomic<uint64_t> layoutAllocateCalls = { 0 };
  std::atomic<uint64_t> layoutAllocateNanos = { 0 };
  std::atomic<uint64_t> layoutTemplateCalls = { 0 };
  std::atomic<uint64_t> layoutTemplateNanos = { 0 };
  std::atomic<uint64_t> renderTextCalls = { 0 };
  std::atomic<uint64_t> renderTextNanos = { 0 };
  std::atomic<uint64_t> renderTextBytes = { 0 };
  std::array<std::atomic<uint64_t>, 2> renderTextRecordCalls = {};
  std::array<std::atomic<uint64_t>, 2> renderTextRecordNanos = {};
  std::array<std::atomic<uint64_t>, 2> renderTextRecordBytes = {};
  std::array<std::atomic<uint64_t>, 2> renderTextRecordOutputBytes = {};
  std::array<std::atomic<uint64_t>, 2> renderTextRecordMaxOutputBytes = {};
  std::atomic<uint64_t> renderBitmapHits = { 0 };
  std::atomic<uint64_t> renderBitmapMisses = { 0 };
  std::atomic<uint64_t> renderBitmapCapacityFallbacks = { 0 };
  std::atomic<uint64_t> renderBitmapReallocations = { 0 };
  std::atomic<uintptr_t> virtualF0Target = { 0 };
  std::atomic<uintptr_t> virtualFinalTarget = { 0 };
  std::atomic<uintptr_t> layoutInputTarget = { 0 };
  std::atomic<uintptr_t> layoutObjectE8Target = { 0 };
  std::atomic<uintptr_t> layoutObjectF0Target = { 0 };
  std::atomic<uintptr_t> layoutObjectC8Target = { 0 };
  std::array<std::atomic<uint64_t>, recordTypeCount> recordCalls = {};
  std::array<std::atomic<uint64_t>, recordTypeCount> recordInclusiveNanos = {};
  std::array<std::atomic<uint64_t>, recordTypeCount> recordExclusiveNanos = {};
};

DeepMenuCounters deepMenu;

const char* baseName(const char* path) {
  const char* back = std::strrchr(path, '\\');
  const char* forward = std::strrchr(path, '/');
  const char* slash = !back || (forward && forward > back) ? forward : back;
  return slash ? slash + 1 : path;
}

DWORD textSectionSize(HMODULE module) {
  auto* base = reinterpret_cast<BYTE*>(module);
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    return 0;
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE)
    return 0;
  auto* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
    if (!std::memcmp(section->Name, ".text", 5))
      return section->Misc.VirtualSize;
  }
  return 0;
}

const char* pssgPath(const void* stringObject) {
  if (!stringObject)
    return nullptr;
  const auto* bytes = static_cast<const BYTE*>(stringObject);
  size_t capacity = 0;
  std::memcpy(&capacity, bytes + 0x18, sizeof(capacity));
  const char* path = nullptr;
  if (capacity >= 0x10)
    std::memcpy(&path, bytes, sizeof(path));
  else
    path = static_cast<const char*>(stringObject);
  if (!path)
    return nullptr;
  const char* extension = std::strrchr(path, '.');
  return extension && !_stricmp(extension, ".PSSG") ? path : nullptr;
}

bool menuStatsEnabled();

bool deepMenuStatsEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_MENU_DEEP_STATS");
    return value && value[0] != '0';
  }();
  return enabled;
}

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

bool battleShadowRestoreEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_BATTLE_SHADOWS");
    return !value || value[0] != '0';   // on by default; "0" disables
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

// §19: trace the engine's by-name shader-uniform setter (0xc61e0) to catch which
// global (gamma/color/exposure/desat) a fullscreen post-effect drives during the
// cut-in — the 54% dim isn't in the ground material's cbuffer (all flat), so it
// must be a post pass. Enabled by ARLAND_UNIFORM_TRACE or, for convenience, the
// same ARLAND_CUTIN_LIGHT_RESTORE the light probes already use.
bool uniformTraceEnabled() {
  static const bool enabled = [] {
    const char* a = std::getenv("ARLAND_UNIFORM_TRACE");
    const char* b = std::getenv("ARLAND_CUTIN_LIGHT_RESTORE");
    return (a && a[0] != '0') || (b && b[0] != '0');
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

// Per-executable-build Rorona addresses used by the battle-shadow machinery
// outside the hook installers. Selected once at detection time.
struct RoronaBuildAddrs {
  const uintptr_t* btlCharaVtables;
  size_t btlCharaVtableCount;
  uintptr_t charaVtable;
  uintptr_t charaBaseVtable;
  uintptr_t eventExecBtlChara;
  uintptr_t eventExecChara;
  uintptr_t managerSlot;       // [gameBase+managerSlot]+0x9d0 = active helper
  uintptr_t battlePublishRet;  // ShadowHelperInit return address, battle setup
  uintptr_t fieldReentryRet;   // ShadowHelperInit return address, field re-entry
};

constexpr RoronaBuildAddrs kRoronaAddrsEn = {
  kBtlCharaVtableRvasEn, std::size(kBtlCharaVtableRvasEn),
  0x74e598, 0x74eb70, 0x76e018, 0x74e3f8,
  0x10c73c8, 0xfe6e1, 0x397307,
};
constexpr RoronaBuildAddrs kRoronaAddrsMulti = {
  kBtlCharaVtableRvasMulti, std::size(kBtlCharaVtableRvasMulti),
  0x76c138, 0x76c710, 0x78bc48, 0x76bf98,
  0x11044c8, 0x106781, 0x3ac8d7,
};

// Null until a Rorona build is recognized; battle-shadow code paths treat that
// as "feature unavailable".
const RoronaBuildAddrs* g_roronaAddrs = nullptr;

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
const BattleStateEntry* g_battleStates = nullptr;
size_t g_battleStateCount = 0;

bool isBattleCharaVtable(uintptr_t vtable) {
  if (!gameBase || !vtable || !g_roronaAddrs)
    return false;
  const uintptr_t rva = vtable - reinterpret_cast<uintptr_t>(gameBase);
  for (size_t i = 0; i < g_roronaAddrs->btlCharaVtableCount; i++)
    if (rva == g_roronaAddrs->btlCharaVtables[i])
      return true;
  return false;
}

// Any renderable character/model family object — BtlChara (battle) or the
// general Chara/CharaBase (field/event/cinematic). Returns a class label or
// nullptr. Used to catch a cut-in character the Event system drives outside the
// BtlChara party vector.
const char* charaFamilyName(uintptr_t vtable) {
  if (!gameBase || !vtable || !g_roronaAddrs)
    return nullptr;
  if (isBattleCharaVtable(vtable))
    return "BtlChara";
  const uintptr_t rva = vtable - reinterpret_cast<uintptr_t>(gameBase);
  if (rva == g_roronaAddrs->charaVtable) return "Chara";
  if (rva == g_roronaAddrs->charaBaseVtable) return "CharaBase";
  return nullptr;
}

// The Event executors that drive a character during a cinematic. Finding one and
// dumping its referenced pointers should reveal the active render node the cut-in
// draws (which is NOT the character's registered [+0x18] node).
const char* eventExecName(uintptr_t vtable) {
  if (!gameBase || !vtable || !g_roronaAddrs)
    return nullptr;
  const uintptr_t rva = vtable - reinterpret_cast<uintptr_t>(gameBase);
  if (rva == g_roronaAddrs->eventExecBtlChara) return "EventExecBtlChara";
  if (rva == g_roronaAddrs->eventExecChara) return "EventExecChara";
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
  const uintptr_t gameMode = helper ? helper - 0x68 : 0;
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
        if (*reinterpret_cast<const uint8_t*>(chara + 0x2d0) != 0)
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

// True if [p, p+n) is committed, readable, non-guard memory.
bool readableRange(uintptr_t p, size_t n) {
  if (!p)
    return false;
  MEMORY_BASIC_INFORMATION mbi = {};
  if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi)))
    return false;
  if (mbi.State != MEM_COMMIT)
    return false;
  const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
    PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  if (!(mbi.Protect & readable) || (mbi.Protect & PAGE_GUARD))
    return false;
  const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  return p >= base && p + n <= base + mbi.RegionSize;
}

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

// Address of the manager's active-helper slot ([manager global]+0x9d0), or
// null. The manager global RVA is per-build (EN 0x10c73c8, multi 0x11044c8);
// both values are pinned by the scenePass install signature, whose RIP
// displacement encodes exactly this slot.
uintptr_t* globalActiveHelperSlot() {
  if (!gameBase || !g_roronaAddrs)
    return nullptr;
  const uintptr_t manager =
    *reinterpret_cast<uintptr_t*>(gameBase + g_roronaAddrs->managerSlot);
  if (!manager)
    return nullptr;
  return reinterpret_cast<uintptr_t*>(manager + 0x9d0);
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
  if (g_roronaAddrs && (callerRva == g_roronaAddrs->battlePublishRet ||
                        callerRva == g_roronaAddrs->fieldReentryRet))
    g_sceneGeneration.fetch_add(1, std::memory_order_release);
  // Track which helper the render path should use: the battle-setup call site
  // publishes the battle helper, the field re-entry call site hands control
  // back to the field helper. Both return addresses are per-build (EN
  // 0xfe6e1/0x397307, multi 0x106781/0x3ac8d7).
  if (g_roronaAddrs && callerRva == g_roronaAddrs->battlePublishRet) {
    const uintptr_t gameMode = helper ? helper - 0x68 : 0;
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
    g_battleActive.store(true, std::memory_order_release);
    atfix::log("==== BATTLE_START ms=", GetTickCount64(),
      " gamemode=", reinterpret_cast<void*>(gameMode),
      " helper=", reinterpret_cast<void*>(helper),
      " scene=", reinterpret_cast<void*>(resource), " ====");
    if (battleShadowRestoreEnabled() && gameMode &&
        locateBattleCharaContainer(gameMode, resource, "init", false)) {
      g_battleContainerFound.store(true, std::memory_order_release);
      registerBattleCharaShadows();
    }
  } else if (g_roronaAddrs && callerRva == g_roronaAddrs->fieldReentryRet) {
    g_battleActive.store(false, std::memory_order_release);
    // Undo a battle-helper publish so the field renders its own helper again.
    const uintptr_t saved =
      g_savedGlobalHelper.exchange(0, std::memory_order_acq_rel);
    if (saved) {
      if (uintptr_t* slot = globalActiveHelperSlot())
        *slot = saved;
    }
  }
  size_t replayed = 0;
  if (battleShadowRestoreEnabled() && g_roronaAddrs &&
      callerRva == g_roronaAddrs->battlePublishRet)
    replayed = dispatchBattleCharaShadows(helper, resource);
  // The per-frame scene shadow pass reads the active helper from the manager
  // global (+0x9d0) and early-outs when it is null; log it here to see whether
  // battle leaves it unset while field publishes it.
  const uintptr_t globalMgr = gameBase && g_roronaAddrs
    ? *reinterpret_cast<const uintptr_t*>(
        gameBase + g_roronaAddrs->managerSlot) : 0;
  const uintptr_t globalActiveHelper = globalMgr
    ? *reinterpret_cast<const uintptr_t*>(globalMgr + 0x9d0) : 0;
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
  const bool alreadyInitialized = actor &&
    *reinterpret_cast<const uint8_t*>(actor + 0x2d0) != 0;
  const uintptr_t gameMode = actor
    ? *reinterpret_cast<const uintptr_t*>(actor + 0x10) : 0;
  const uintptr_t character = actor
    ? *reinterpret_cast<const uintptr_t*>(actor + 0x18) : 0;
  const uintptr_t helper = gameMode ? gameMode + 0x68 : 0;
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

void rememberTarget(std::atomic<uintptr_t>& destination, uintptr_t target) {
  uintptr_t empty = 0;
  destination.compare_exchange_strong(
    empty, target, std::memory_order_relaxed);
}

uintptr_t timedNodeInit(uintptr_t node, uintptr_t owner,
                        uintptr_t record, uintptr_t id) {
  if (node) {
    const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(node);
    if (vtable) {
      rememberTarget(deepMenu.virtualF0Target,
        *reinterpret_cast<const uintptr_t*>(vtable + 0xf0));
      rememberTarget(deepMenu.virtualFinalTarget,
        *reinterpret_cast<const uintptr_t*>(vtable + 0x38));
    }
  }
  const auto started = std::chrono::steady_clock::now();
  ++nodeInitDepth;
  const uintptr_t result = originalNodeInit(node, owner, record, id);
  --nodeInitDepth;
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  deepMenu.nodeCalls.fetch_add(1, std::memory_order_relaxed);
  deepMenu.nodeNanos.fetch_add(uint64_t(elapsed), std::memory_order_relaxed);
  return result;
}

uintptr_t timedNodeResource(uintptr_t object, uintptr_t name,
                            uintptr_t resource, uintptr_t unused) {
  if (!nodeInitDepth)
    return originalNodeResource(object, name, resource, unused);
  const auto started = std::chrono::steady_clock::now();
  const uintptr_t result = originalNodeResource(object, name, resource, unused);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  deepMenu.resourceCalls.fetch_add(1, std::memory_order_relaxed);
  deepMenu.resourceNanos.fetch_add(uint64_t(elapsed), std::memory_order_relaxed);
  return result;
}

uintptr_t timedNodeLayout(uintptr_t a1, uintptr_t a2, uintptr_t a3,
                          uintptr_t a4, uintptr_t a5, uintptr_t a6,
                          uintptr_t a7, uintptr_t a8, uintptr_t a9) {
  if (!nodeInitDepth)
    return originalNodeLayout(a1, a2, a3, a4, a5, a6, a7, a8, a9);
  const auto started = std::chrono::steady_clock::now();
  const uintptr_t result = originalNodeLayout(
    a1, a2, a3, a4, a5, a6, a7, a8, a9);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  deepMenu.layoutCalls.fetch_add(1, std::memory_order_relaxed);
  deepMenu.layoutNanos.fetch_add(uint64_t(elapsed), std::memory_order_relaxed);
  return result;
}

uintptr_t timedRecord(uintptr_t context, uintptr_t record,
                      uintptr_t id, uintptr_t extra) {
  if (!recordTimingActive.load(std::memory_order_relaxed))
    return originalRecord(context, record, id, extra);

  const int type = record
    ? *reinterpret_cast<const int*>(record + 0x10) : -1;
  const auto started = std::chrono::steady_clock::now();
  const size_t depth = recordDepth++;
  const bool isTextRecord = type == 19 || type == 20;
  const int previousTextRecordType = activeTextRecordType;
  if (isTextRecord) {
    ++type19Depth;
    activeTextRecordType = type;
  }
  if (depth < recordDepthLimit)
    recordChildNanos[depth] = 0;
  const uintptr_t result = originalRecord(context, record, id, extra);
  if (isTextRecord) {
    activeTextRecordType = previousTextRecordType;
    --type19Depth;
  }
  const uint64_t elapsed = uint64_t(
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - started).count());
  uint64_t exclusive = elapsed;
  if (depth < recordDepthLimit) {
    const uint64_t children = recordChildNanos[depth];
    exclusive = children < exclusive ? exclusive - children : 0;
  }
  --recordDepth;
  if (depth > 0 && depth - 1 < recordDepthLimit)
    recordChildNanos[depth - 1] += elapsed;

  if (type >= 0 && size_t(type) < recordTypeCount) {
    deepMenu.recordCalls[type].fetch_add(1, std::memory_order_relaxed);
    deepMenu.recordInclusiveNanos[type].fetch_add(
      elapsed, std::memory_order_relaxed);
    deepMenu.recordExclusiveNanos[type].fetch_add(
      exclusive, std::memory_order_relaxed);
  }
  return result;
}

uintptr_t timedLayoutLookup(uintptr_t a1, uintptr_t a2,
                            uintptr_t a3, uintptr_t a4) {
  if (!nodeInitDepth)
    return originalLayoutLookup(a1, a2, a3, a4);
  const auto started = std::chrono::steady_clock::now();
  const uintptr_t result = originalLayoutLookup(a1, a2, a3, a4);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  deepMenu.layoutLookupCalls.fetch_add(1, std::memory_order_relaxed);
  deepMenu.layoutLookupNanos.fetch_add(uint64_t(elapsed), std::memory_order_relaxed);
  return result;
}

uintptr_t timedLayoutCreate(uintptr_t a1, uintptr_t a2, uintptr_t a3,
                            uintptr_t a4, uintptr_t a5, uintptr_t a6) {
  if (!nodeInitDepth)
    return originalLayoutCreate(a1, a2, a3, a4, a5, a6);
  const auto started = std::chrono::steady_clock::now();
  ++layoutCreateDepth;
  const uintptr_t result = originalLayoutCreate(a1, a2, a3, a4, a5, a6);
  --layoutCreateDepth;
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  deepMenu.layoutCreateCalls.fetch_add(1, std::memory_order_relaxed);
  deepMenu.layoutCreateNanos.fetch_add(uint64_t(elapsed), std::memory_order_relaxed);
  return result;
}

uintptr_t timedLayoutApply(uintptr_t a1, uintptr_t a2, uintptr_t a3,
                           uintptr_t a4, uintptr_t a5) {
  if (!nodeInitDepth)
    return originalLayoutApply(a1, a2, a3, a4, a5);
  if (a3) {
    const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(a3);
    if (vtable)
      rememberTarget(deepMenu.layoutInputTarget,
        *reinterpret_cast<const uintptr_t*>(vtable + 0x48));
  }
  const auto started = std::chrono::steady_clock::now();
  ++layoutApplyDepth;
  const uintptr_t result = originalLayoutApply(a1, a2, a3, a4, a5);
  --layoutApplyDepth;
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  if (a1) {
    const uintptr_t object = *reinterpret_cast<const uintptr_t*>(a1 + 0x38);
    if (object) {
      const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(object);
      if (vtable) {
        rememberTarget(deepMenu.layoutObjectE8Target,
          *reinterpret_cast<const uintptr_t*>(vtable + 0xe8));
        rememberTarget(deepMenu.layoutObjectF0Target,
          *reinterpret_cast<const uintptr_t*>(vtable + 0xf0));
        rememberTarget(deepMenu.layoutObjectC8Target,
          *reinterpret_cast<const uintptr_t*>(vtable + 0xc8));
      }
    }
  }
  deepMenu.layoutApplyCalls.fetch_add(1, std::memory_order_relaxed);
  deepMenu.layoutApplyNanos.fetch_add(uint64_t(elapsed), std::memory_order_relaxed);
  return result;
}

uintptr_t timedLayoutBuildCore(uintptr_t a1, uintptr_t a2, uintptr_t a3,
                               uintptr_t a4, uintptr_t a5, uintptr_t a6,
                               uintptr_t a7, uintptr_t a8, uintptr_t a9) {
  if (!layoutCreateDepth)
    return originalLayoutBuildCore(a1, a2, a3, a4, a5, a6, a7, a8, a9);
  const auto started = std::chrono::steady_clock::now();
  const uintptr_t result = originalLayoutBuildCore(
    a1, a2, a3, a4, a5, a6, a7, a8, a9);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  deepMenu.layoutBuildCoreCalls.fetch_add(1, std::memory_order_relaxed);
  deepMenu.layoutBuildCoreNanos.fetch_add(
    uint64_t(elapsed), std::memory_order_relaxed);
  uint64_t key = 0xcbf29ce484222325ULL;
  const std::array<uintptr_t, 8> inputs = {
    a1, a2, a3, a4, a5, a6, a7, a8,
  };
  for (const uintptr_t input : inputs) {
    key ^= uint64_t(input);
    key *= 0x100000001b3ULL;
  }
  layoutBuildKeys.insert(key);
  if (result)
    layoutBuildResults.insert(result);
  return result;
}

uintptr_t timedLayoutEntryInit(uintptr_t a1, uintptr_t a2, uintptr_t a3) {
  if (!layoutCreateDepth)
    return originalLayoutEntryInit(a1, a2, a3);
  const auto started = std::chrono::steady_clock::now();
  const uintptr_t result = originalLayoutEntryInit(a1, a2, a3);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  deepMenu.layoutEntryInitCalls.fetch_add(1, std::memory_order_relaxed);
  deepMenu.layoutEntryInitNanos.fetch_add(
    uint64_t(elapsed), std::memory_order_relaxed);
  return result;
}

uintptr_t timedLayoutAcquire(uintptr_t a1, uintptr_t a2, uintptr_t a3) {
  if (!layoutApplyDepth)
    return originalLayoutAcquire(a1, a2, a3);
  const auto started = std::chrono::steady_clock::now();
  const uintptr_t result = originalLayoutAcquire(a1, a2, a3);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  deepMenu.layoutAcquireCalls.fetch_add(1, std::memory_order_relaxed);
  deepMenu.layoutAcquireNanos.fetch_add(
    uint64_t(elapsed), std::memory_order_relaxed);
  return result;
}

uintptr_t timedLayoutApplyCore(uintptr_t a1, uintptr_t a2, uintptr_t a3) {
  if (!layoutApplyDepth)
    return originalLayoutApplyCore(a1, a2, a3);
  const auto started = std::chrono::steady_clock::now();
  const uintptr_t result = originalLayoutApplyCore(a1, a2, a3);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  deepMenu.layoutApplyCoreCalls.fetch_add(1, std::memory_order_relaxed);
  deepMenu.layoutApplyCoreNanos.fetch_add(
    uint64_t(elapsed), std::memory_order_relaxed);
  return result;
}

uintptr_t timedLayoutVirtualCall(LayoutVirtualProc original,
                                 std::atomic<uint64_t>& calls,
                                 std::atomic<uint64_t>& nanos,
                                 uintptr_t a1, uintptr_t a2, uintptr_t a3) {
  if (!layoutApplyDepth)
    return original(a1, a2, a3);
  const auto started = std::chrono::steady_clock::now();
  const uintptr_t result = original(a1, a2, a3);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  calls.fetch_add(1, std::memory_order_relaxed);
  nanos.fetch_add(uint64_t(elapsed), std::memory_order_relaxed);
  return result;
}

uintptr_t timedLayoutObjectE8(uintptr_t a1, uintptr_t a2, uintptr_t a3) {
  return timedLayoutVirtualCall(originalLayoutObjectE8,
    deepMenu.layoutObjectE8Calls, deepMenu.layoutObjectE8Nanos,
    a1, a2, a3);
}

uintptr_t timedLayoutObjectF0(uintptr_t a1, uintptr_t a2, uintptr_t a3) {
  return timedLayoutVirtualCall(originalLayoutObjectF0,
    deepMenu.layoutObjectF0Calls, deepMenu.layoutObjectF0Nanos,
    a1, a2, a3);
}

uintptr_t timedLayoutObjectC8(uintptr_t a1, uintptr_t a2, uintptr_t a3) {
  return timedLayoutVirtualCall(originalLayoutObjectC8,
    deepMenu.layoutObjectC8Calls, deepMenu.layoutObjectC8Nanos,
    a1, a2, a3);
}

uintptr_t timedLayoutValidateC8(uintptr_t a1, uintptr_t a2, uintptr_t a3) {
  return timedLayoutVirtualCall(originalLayoutValidateC8,
    deepMenu.layoutValidateC8Calls, deepMenu.layoutValidateC8Nanos,
    a1, a2, a3);
}

uintptr_t timedLayoutValidateF0(uintptr_t a1, uintptr_t a2,
                                uintptr_t a3, uintptr_t a4) {
  if (!layoutApplyDepth)
    return originalLayoutValidateF0(a1, a2, a3, a4);
  const auto started = std::chrono::steady_clock::now();
  const uintptr_t result = originalLayoutValidateF0(a1, a2, a3, a4);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  deepMenu.layoutValidateF0Calls.fetch_add(1, std::memory_order_relaxed);
  deepMenu.layoutValidateF0Nanos.fetch_add(
    uint64_t(elapsed), std::memory_order_relaxed);
  return result;
}

uintptr_t timedLayoutRebuild(uintptr_t a1, uintptr_t a2) {
  if (!layoutApplyDepth)
    return originalLayoutRebuild(a1, a2);
  const auto started = std::chrono::steady_clock::now();
  const uintptr_t result = originalLayoutRebuild(a1, a2);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  deepMenu.layoutRebuildCalls.fetch_add(1, std::memory_order_relaxed);
  deepMenu.layoutRebuildNanos.fetch_add(
    uint64_t(elapsed), std::memory_order_relaxed);
  return result;
}

uintptr_t timedLayoutAllocate(uintptr_t a1, uintptr_t a2, uintptr_t a3) {
  if (!layoutCreateDepth)
    return originalLayoutAllocate(a1, a2, a3);
  const auto started = std::chrono::steady_clock::now();
  const uintptr_t result = originalLayoutAllocate(a1, a2, a3);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  deepMenu.layoutAllocateCalls.fetch_add(1, std::memory_order_relaxed);
  deepMenu.layoutAllocateNanos.fetch_add(
    uint64_t(elapsed), std::memory_order_relaxed);
  return result;
}

uintptr_t timedLayoutTemplate(uintptr_t a1, uintptr_t a2, uintptr_t a3,
                              uintptr_t a4, uintptr_t a5, uintptr_t a6,
                              uintptr_t a7, uintptr_t a8, uintptr_t a9) {
  if (!layoutCreateDepth)
    return originalLayoutTemplate(a1, a2, a3, a4, a5, a6, a7, a8, a9);
  const auto started = std::chrono::steady_clock::now();
  const uintptr_t result = originalLayoutTemplate(
    a1, a2, a3, a4, a5, a6, a7, a8, a9);
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now() - started).count();
  deepMenu.layoutTemplateCalls.fetch_add(1, std::memory_order_relaxed);
  deepMenu.layoutTemplateNanos.fetch_add(
    uint64_t(elapsed), std::memory_order_relaxed);
  if (result)
    layoutTemplateResults.insert(result);
  return result;
}

bool cachedPathCheck(void* context, void* pathString) {
  const char* path = pssgPath(pathString);
  if (!path)
    return originalPathCheck(context, pathString);
  const std::string key(path);
  {
    std::lock_guard lock(cacheMutex);
    if (successfulPaths.find(key) != successfulPaths.end()) {
      if (menuStatsEnabled())
        pathCacheHits.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  if (menuStatsEnabled())
    pathRealChecks.fetch_add(1, std::memory_order_relaxed);
  if (!originalPathCheck(context, pathString))
    return false;
  std::lock_guard lock(cacheMutex);
  successfulPaths.insert(key);
  return true;
}

bool atlasCacheEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_ATLAS_CACHE");
    return !value || value[0] != '0';
  }();
  return enabled;
}

bool frameAtlasCacheEnabled() {
  static const int overrideValue = [] {
    const char* value = std::getenv("ARLAND_FRAME_ATLAS_CACHE");
    return value ? (value[0] != '0' ? 1 : 0) : -1;
  }();
  return overrideValue >= 0
    ? overrideValue != 0
    : frameAtlasCacheDefault.load(std::memory_order_relaxed);
}

bool textBitmapCacheEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_TEXT_BITMAP_CACHE");
    return value && value[0] != '0';
  }();
  return enabled;
}

bool menuStatsEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_MENU_STATS");
    return value && value[0] != '0';
  }();
  return enabled;
}

bool menuTransitionTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_MENU_TRANSITION_TRACE");
    return value && value[0] != '0';
  }();
  return enabled;
}

std::atomic<uint32_t> transitionSequence = 0;
std::atomic<uint32_t> transitionPresentBudget = 0;
std::atomic<uint32_t> transitionPresentIndex = 0;
std::atomic<uint64_t> transitionDrainMicros = 0;

void cachedQueueDrain(void* manager) {
  const bool outermost =
    atlasDrainDepth.fetch_add(1, std::memory_order_acq_rel) == 0;
  if (outermost && !frameAtlasCacheEnabled()) {
    std::lock_guard lock(atlasMutex);
    atlasReads.clear();
    atlasCacheActive.store(true, std::memory_order_release);
  }
  if (outermost && textBitmapCacheEnabled()) {
    std::lock_guard lock(renderBitmapMutex);
    renderBitmapCache.clear();
    renderBitmapCache.reserve(256);
  }

  const bool stats = outermost && menuStatsEnabled();
  const uint64_t pathHitsBefore = stats
    ? pathCacheHits.load(std::memory_order_relaxed) : 0;
  const uint64_t pathChecksBefore = stats
    ? pathRealChecks.load(std::memory_order_relaxed) : 0;
  const uint64_t atlasHitsBefore = stats
    ? atlasCacheHits.load(std::memory_order_relaxed) : 0;
  const uint64_t atlasReadsBefore = stats
    ? atlasRealReads.load(std::memory_order_relaxed) : 0;
  const auto started = std::chrono::steady_clock::now();

  if (stats && deepMenuStatsEnabled()) {
    deepMenu.nodeCalls.store(0, std::memory_order_relaxed);
    deepMenu.nodeNanos.store(0, std::memory_order_relaxed);
    deepMenu.resourceCalls.store(0, std::memory_order_relaxed);
    deepMenu.resourceNanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutCalls.store(0, std::memory_order_relaxed);
    deepMenu.layoutNanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutLookupCalls.store(0, std::memory_order_relaxed);
    deepMenu.layoutLookupNanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutCreateCalls.store(0, std::memory_order_relaxed);
    deepMenu.layoutCreateNanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutApplyCalls.store(0, std::memory_order_relaxed);
    deepMenu.layoutApplyNanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutBuildCoreCalls.store(0, std::memory_order_relaxed);
    deepMenu.layoutBuildCoreNanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutEntryInitCalls.store(0, std::memory_order_relaxed);
    deepMenu.layoutEntryInitNanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutAcquireCalls.store(0, std::memory_order_relaxed);
    deepMenu.layoutAcquireNanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutApplyCoreCalls.store(0, std::memory_order_relaxed);
    deepMenu.layoutApplyCoreNanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutObjectE8Calls.store(0, std::memory_order_relaxed);
    deepMenu.layoutObjectE8Nanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutObjectF0Calls.store(0, std::memory_order_relaxed);
    deepMenu.layoutObjectF0Nanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutObjectC8Calls.store(0, std::memory_order_relaxed);
    deepMenu.layoutObjectC8Nanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutValidateC8Calls.store(0, std::memory_order_relaxed);
    deepMenu.layoutValidateC8Nanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutValidateF0Calls.store(0, std::memory_order_relaxed);
    deepMenu.layoutValidateF0Nanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutRebuildCalls.store(0, std::memory_order_relaxed);
    deepMenu.layoutRebuildNanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutAllocateCalls.store(0, std::memory_order_relaxed);
    deepMenu.layoutAllocateNanos.store(0, std::memory_order_relaxed);
    deepMenu.layoutTemplateCalls.store(0, std::memory_order_relaxed);
    deepMenu.layoutTemplateNanos.store(0, std::memory_order_relaxed);
    deepMenu.renderTextCalls.store(0, std::memory_order_relaxed);
    deepMenu.renderTextNanos.store(0, std::memory_order_relaxed);
    deepMenu.renderTextBytes.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < 2; i++) {
      deepMenu.renderTextRecordCalls[i].store(0, std::memory_order_relaxed);
      deepMenu.renderTextRecordNanos[i].store(0, std::memory_order_relaxed);
      deepMenu.renderTextRecordBytes[i].store(0, std::memory_order_relaxed);
      deepMenu.renderTextRecordOutputBytes[i].store(0,
        std::memory_order_relaxed);
      deepMenu.renderTextRecordMaxOutputBytes[i].store(0,
        std::memory_order_relaxed);
      renderTextExactKeysByRecordType[i].clear();
      renderTextExactKeysByRecordType[i].reserve(64);
    }
    deepMenu.renderBitmapHits.store(0, std::memory_order_relaxed);
    deepMenu.renderBitmapMisses.store(0, std::memory_order_relaxed);
    deepMenu.renderBitmapCapacityFallbacks.store(0, std::memory_order_relaxed);
    deepMenu.renderBitmapReallocations.store(0, std::memory_order_relaxed);
    layoutBuildKeys.clear();
    layoutBuildResults.clear();
    layoutTemplateResults.clear();
    renderTextKeys.clear();
    renderTextExactKeys.clear();
    renderTextPointers.clear();
    renderTextRenderers.clear();
    {
      std::lock_guard bitmapLock(renderBitmapMutex);
      renderOutputSignatures.clear();
      renderKeyTimings.clear();
      renderOutputRepeatMatches = 0;
      renderOutputRepeatConflicts = 0;
      renderOutputInvalidSamples = 0;
    }
    layoutBuildKeys.reserve(1024);
    layoutBuildResults.reserve(1024);
    layoutTemplateResults.reserve(1024);
    renderTextKeys.reserve(256);
    renderTextExactKeys.reserve(256);
    renderTextPointers.reserve(256);
    renderTextRenderers.reserve(32);
    deepMenu.virtualF0Target.store(0, std::memory_order_relaxed);
    deepMenu.virtualFinalTarget.store(0, std::memory_order_relaxed);
    deepMenu.layoutInputTarget.store(0, std::memory_order_relaxed);
    deepMenu.layoutObjectE8Target.store(0, std::memory_order_relaxed);
    deepMenu.layoutObjectF0Target.store(0, std::memory_order_relaxed);
    deepMenu.layoutObjectC8Target.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < recordTypeCount; i++) {
      deepMenu.recordCalls[i].store(0, std::memory_order_relaxed);
      deepMenu.recordInclusiveNanos[i].store(0, std::memory_order_relaxed);
      deepMenu.recordExclusiveNanos[i].store(0, std::memory_order_relaxed);
    }
    recordTimingActive.store(true, std::memory_order_release);
  }

  originalQueueDrain(manager);

  if (stats && deepMenuStatsEnabled())
    recordTimingActive.store(false, std::memory_order_release);

  if (atlasDrainDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    if (!frameAtlasCacheEnabled()) {
      atlasCacheActive.store(false, std::memory_order_release);
      {
        std::lock_guard lock(atlasMutex);
        atlasReads.clear();
      }
    }
    if (textBitmapCacheEnabled()) {
      std::lock_guard bitmapLock(renderBitmapMutex);
      renderBitmapCache.clear();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - started).count();
    if (menuTransitionTraceEnabled() && elapsed >= 10000) {
      const uint32_t sequence = transitionSequence.fetch_add(
        1, std::memory_order_relaxed) + 1;
      transitionDrainMicros.store(uint64_t(elapsed), std::memory_order_relaxed);
      transitionPresentIndex.store(0, std::memory_order_relaxed);
      transitionPresentBudget.store(12, std::memory_order_release);
      atfix::log("TRANSITION drain id=", sequence,
        " us=", elapsed, " thread=", GetCurrentThreadId());
    }
    if (stats) {
      const uint64_t pathHitDelta =
        pathCacheHits.load(std::memory_order_relaxed) - pathHitsBefore;
      const uint64_t pathRealDelta =
        pathRealChecks.load(std::memory_order_relaxed) - pathChecksBefore;
      const uint64_t atlasHitDelta =
        atlasCacheHits.load(std::memory_order_relaxed) - atlasHitsBefore;
      const uint64_t atlasRealDelta =
        atlasRealReads.load(std::memory_order_relaxed) - atlasReadsBefore;
      if (elapsed < 1000 && !pathHitDelta && !pathRealDelta &&
          !atlasHitDelta && !atlasRealDelta)
        return;
      atfix::log("MENU drain us=", elapsed,
        " pssg_cached=", pathHitDelta,
        " pssg_real=", pathRealDelta,
        " atlas_cached=", atlasHitDelta,
        " atlas_real=", atlasRealDelta);
      if (deepMenuStatsEnabled()) {
        const uintptr_t f0 = deepMenu.virtualF0Target.load(
          std::memory_order_relaxed);
        const uintptr_t final = deepMenu.virtualFinalTarget.load(
          std::memory_order_relaxed);
        atfix::log("MENU deep node calls=",
          deepMenu.nodeCalls.load(std::memory_order_relaxed),
          " us=", deepMenu.nodeNanos.load(std::memory_order_relaxed) / 1000,
          " resource calls=", deepMenu.resourceCalls.load(std::memory_order_relaxed),
          " us=", deepMenu.resourceNanos.load(std::memory_order_relaxed) / 1000,
          " layout calls=", deepMenu.layoutCalls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutNanos.load(std::memory_order_relaxed) / 1000,
          " vf0_rva=0x", std::hex,
          f0 && gameBase ? f0 - reinterpret_cast<uintptr_t>(gameBase) : 0,
          " vfinal_rva=0x",
          final && gameBase ? final - reinterpret_cast<uintptr_t>(gameBase) : 0,
          std::dec);
        atfix::log("MENU layout lookup calls=",
          deepMenu.layoutLookupCalls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutLookupNanos.load(std::memory_order_relaxed) / 1000,
          " create calls=", deepMenu.layoutCreateCalls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutCreateNanos.load(std::memory_order_relaxed) / 1000,
          " apply calls=", deepMenu.layoutApplyCalls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutApplyNanos.load(std::memory_order_relaxed) / 1000);
        atfix::log("MENU layout core-build calls=",
          deepMenu.layoutBuildCoreCalls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutBuildCoreNanos.load(std::memory_order_relaxed) / 1000,
          " entry-init calls=",
          deepMenu.layoutEntryInitCalls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutEntryInitNanos.load(std::memory_order_relaxed) / 1000,
          " acquire calls=",
          deepMenu.layoutAcquireCalls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutAcquireNanos.load(std::memory_order_relaxed) / 1000,
          " apply-core calls=",
          deepMenu.layoutApplyCoreCalls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutApplyCoreNanos.load(std::memory_order_relaxed) / 1000);
        atfix::log("MENU layout virtual-e8 calls=",
          deepMenu.layoutObjectE8Calls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutObjectE8Nanos.load(std::memory_order_relaxed) / 1000,
          " virtual-f0 calls=",
          deepMenu.layoutObjectF0Calls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutObjectF0Nanos.load(std::memory_order_relaxed) / 1000,
          " virtual-c8 calls=",
          deepMenu.layoutObjectC8Calls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutObjectC8Nanos.load(std::memory_order_relaxed) / 1000);
        atfix::log("MENU layout validate-c8 calls=",
          deepMenu.layoutValidateC8Calls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutValidateC8Nanos.load(std::memory_order_relaxed) / 1000,
          " validate-f0 calls=",
          deepMenu.layoutValidateF0Calls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutValidateF0Nanos.load(std::memory_order_relaxed) / 1000,
          " rebuild calls=",
          deepMenu.layoutRebuildCalls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutRebuildNanos.load(std::memory_order_relaxed) / 1000);
        atfix::log("MENU layout build-tuples calls=",
          deepMenu.layoutBuildCoreCalls.load(std::memory_order_relaxed),
          " unique_keys=", layoutBuildKeys.size(),
          " unique_results=", layoutBuildResults.size());
        atfix::log("MENU layout allocate calls=",
          deepMenu.layoutAllocateCalls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutAllocateNanos.load(std::memory_order_relaxed) / 1000,
          " template calls=",
          deepMenu.layoutTemplateCalls.load(std::memory_order_relaxed),
          " us=", deepMenu.layoutTemplateNanos.load(std::memory_order_relaxed) / 1000,
          " unique_template_results=", layoutTemplateResults.size());
        atfix::log("MENU text-render calls=",
          deepMenu.renderTextCalls.load(std::memory_order_relaxed),
          " us=", deepMenu.renderTextNanos.load(std::memory_order_relaxed) / 1000,
          " bytes=", deepMenu.renderTextBytes.load(std::memory_order_relaxed),
          " coarse_unique_keys=", renderTextKeys.size(),
          " exact_unique_keys=", renderTextExactKeys.size(),
          " unique_string_pointers=", renderTextPointers.size(),
          " unique_renderers=", renderTextRenderers.size());
        for (size_t i = 0; i < 2; i++) {
          atfix::log("MENU text-render record_type=", i + 19,
            " calls=",
            deepMenu.renderTextRecordCalls[i].load(std::memory_order_relaxed),
            " us=",
            deepMenu.renderTextRecordNanos[i].load(
              std::memory_order_relaxed) / 1000,
            " bytes=",
            deepMenu.renderTextRecordBytes[i].load(
              std::memory_order_relaxed),
            " output_bytes=",
            deepMenu.renderTextRecordOutputBytes[i].load(
              std::memory_order_relaxed),
            " max_output_bytes=",
            deepMenu.renderTextRecordMaxOutputBytes[i].load(
              std::memory_order_relaxed),
            " unique_keys=", renderTextExactKeysByRecordType[i].size());
        }
        atfix::log("MENU text-bitmap-cache hits=",
          deepMenu.renderBitmapHits.load(std::memory_order_relaxed),
          " misses=", deepMenu.renderBitmapMisses.load(std::memory_order_relaxed),
          " capacity_fallbacks=",
          deepMenu.renderBitmapCapacityFallbacks.load(std::memory_order_relaxed),
          " reallocations=",
          deepMenu.renderBitmapReallocations.load(std::memory_order_relaxed));
        {
          std::lock_guard bitmapLock(renderBitmapMutex);
          atfix::log("MENU text-output unique=", renderOutputSignatures.size(),
            " repeat_matches=", renderOutputRepeatMatches,
            " repeat_conflicts=", renderOutputRepeatConflicts,
            " invalid_samples=", renderOutputInvalidSamples);
          for (const auto& [key, timing] : renderKeyTimings) {
            if (timing.recordType != 20)
              continue;
            std::string preview = key.text.substr(0, 96);
            for (char& character : preview) {
              const unsigned char value = static_cast<unsigned char>(character);
              if (value < 0x20 || value > 0x7e)
                character = '?';
            }
            atfix::log("MENU text-key record_type=20 calls=", timing.calls,
              " total_us=", timing.totalNanos / 1000,
              " min_us=", timing.minimumNanos / 1000,
              " max_us=", timing.maximumNanos / 1000,
              " length=", key.text.size(), " text=\"", preview, "\"");
          }
        }
        const uintptr_t inputTarget = deepMenu.layoutInputTarget.load(
          std::memory_order_relaxed);
        const uintptr_t objectE8 = deepMenu.layoutObjectE8Target.load(
          std::memory_order_relaxed);
        const uintptr_t objectF0 = deepMenu.layoutObjectF0Target.load(
          std::memory_order_relaxed);
        const uintptr_t objectC8 = deepMenu.layoutObjectC8Target.load(
          std::memory_order_relaxed);
        atfix::log("MENU layout targets input48_rva=0x", std::hex,
          inputTarget && gameBase
            ? inputTarget - reinterpret_cast<uintptr_t>(gameBase) : 0,
          " object_e8_rva=0x", objectE8 && gameBase
            ? objectE8 - reinterpret_cast<uintptr_t>(gameBase) : 0,
          " object_f0_rva=0x", objectF0 && gameBase
            ? objectF0 - reinterpret_cast<uintptr_t>(gameBase) : 0,
          " object_c8_rva=0x", objectC8 && gameBase
            ? objectC8 - reinterpret_cast<uintptr_t>(gameBase) : 0,
          std::dec);
        for (size_t i = 0; i < recordTypeCount; i++) {
          const uint64_t calls = deepMenu.recordCalls[i].load(
            std::memory_order_relaxed);
          if (calls)
            atfix::log("MENU record type=", i, " calls=", calls,
              " inclusive_us=",
              deepMenu.recordInclusiveNanos[i].load(
                std::memory_order_relaxed) / 1000,
              " exclusive_us=",
              deepMenu.recordExclusiveNanos[i].load(
                std::memory_order_relaxed) / 1000);
        }
      }
    }
  }
}

uintptr_t cachedRenderText(uintptr_t a, uintptr_t b,
                           uintptr_t c, uintptr_t d) {
  const bool profile = type19Depth && deepMenuStatsEnabled();
  const bool cache = textBitmapCacheEnabled() &&
    atlasCacheActive.load(std::memory_order_acquire) && gameAlloc && gameFree &&
    a && b;
  uint64_t key = 0xcbf29ce484222325ULL;
  uint64_t exactKey = 0;
  size_t length = 0;
  uintptr_t font = 0;
  uintptr_t atlas = 0;
  uint8_t variant = 0;
  uint32_t mode = 0;
  if ((profile || cache) && b) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(b);
    while (length < 4096 && bytes[length]) {
      key ^= bytes[length++];
      key *= 0x100000001b3ULL;
    }
    key ^= uint64_t(c);
    key *= 0x100000001b3ULL;
    exactKey = key;
    const auto* renderer = reinterpret_cast<const BYTE*>(a);
    if (renderer) {
      std::memcpy(&font, renderer + 0x1b8, sizeof(font));
      std::memcpy(&atlas, renderer + 0x1c0, sizeof(atlas));
      std::memcpy(&variant, renderer + 0x1c8, sizeof(variant));
      std::memcpy(&mode, renderer + 0x1cc, sizeof(mode));
    }
    const std::array<uintptr_t, 5> style = {
      a, font, atlas, uintptr_t(variant), uintptr_t(mode),
    };
    for (const uintptr_t value : style) {
      exactKey ^= uint64_t(value);
      exactKey *= 0x100000001b3ULL;
    }
  }

  RenderTextKey cacheKey;
  const bool validSemanticKey = (profile || cache) && a && b && length < 4096;
  const bool validCacheKey = cache && validSemanticKey;
  if (validSemanticKey) {
    cacheKey.renderer = a;
    cacheKey.font = font;
    cacheKey.atlas = atlas;
    cacheKey.mode = mode;
    cacheKey.variant = variant;
    cacheKey.flag = uint8_t(c != 0);
    cacheKey.text.assign(reinterpret_cast<const char*>(b), length);
  }

  const auto started = profile
    ? std::chrono::steady_clock::now()
    : std::chrono::steady_clock::time_point{};
  uintptr_t result = 0;
  bool replayed = false;
  if (validCacheKey) {
    std::lock_guard lock(renderBitmapMutex);
    const auto found = renderBitmapCache.find(cacheKey);
    if (found != renderBitmapCache.end()) {
      const auto* renderer = reinterpret_cast<const BYTE*>(a);
      uintptr_t outputAddress = 0;
      std::memcpy(&outputAddress, renderer + 0x1a0, sizeof(outputAddress));
      auto* output = reinterpret_cast<BYTE*>(outputAddress);
      int32_t currentWidth = 0;
      int32_t currentHeight = 0;
      uintptr_t pixelsAddress = 0;
      if (output) {
        std::memcpy(&currentWidth, output, sizeof(currentWidth));
        std::memcpy(&currentHeight, output + 4, sizeof(currentHeight));
        std::memcpy(&pixelsAddress, output + 8, sizeof(pixelsAddress));
      }
      const uint64_t capacity = currentWidth > 0 && currentHeight > 0
        ? uint64_t(uint32_t(currentWidth)) * uint32_t(currentHeight) : 0;
      const auto& bitmap = found->second;
      uint64_t available = capacity;
      if (output && (!pixelsAddress || available < bitmap.bytes.size()) &&
          !bitmap.bytes.empty()) {
        void* replacement = gameAlloc(bitmap.bytes.size());
        if (replacement) {
          gameFree(reinterpret_cast<void*>(pixelsAddress));
          pixelsAddress = reinterpret_cast<uintptr_t>(replacement);
          std::memcpy(output + 8, &pixelsAddress, sizeof(pixelsAddress));
          available = bitmap.bytes.size();
          deepMenu.renderBitmapReallocations.fetch_add(
            1, std::memory_order_relaxed);
        }
      }
      if (output && pixelsAddress && available >= bitmap.bytes.size() &&
          !bitmap.bytes.empty()) {
        std::memcpy(reinterpret_cast<void*>(pixelsAddress),
          bitmap.bytes.data(), bitmap.bytes.size());
        std::memcpy(output, &bitmap.width, sizeof(bitmap.width));
        std::memcpy(output + 4, &bitmap.height, sizeof(bitmap.height));
        std::memcpy(output + 0x10, bitmap.metrics.data(),
          sizeof(bitmap.metrics));
        const uint32_t ready = 1;
        std::memcpy(output + 0x20, &ready, sizeof(ready));
        result = bitmap.result;
        replayed = true;
        deepMenu.renderBitmapHits.fetch_add(1, std::memory_order_relaxed);
      } else {
        deepMenu.renderBitmapCapacityFallbacks.fetch_add(
          1, std::memory_order_relaxed);
      }
    } else {
      deepMenu.renderBitmapMisses.fetch_add(1, std::memory_order_relaxed);
    }
  }

  if (!replayed) {
    const ActiveRenderTrace previousRenderTrace = activeRenderTrace;
    if (profile)
      activeRenderTrace = { true, 0, 0, 0, 0 };
    ++renderTextDepth;
    result = originalRenderText(a, b, c, d);
    --renderTextDepth;
    const ActiveRenderTrace completedRenderTrace = activeRenderTrace;
    activeRenderTrace = previousRenderTrace;

    if (profile && validSemanticKey) {
      const uint64_t elapsed = uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - started).count());
      if (activeTextRecordType == 20 && elapsed >= 1000000) {
        std::string preview = cacheKey.text.substr(0, 96);
        for (char& character : preview) {
          const unsigned char value = static_cast<unsigned char>(character);
          if (value < 0x20 || value > 0x7e)
            character = '?';
        }
        atfix::log("MENU slow-text record_type=20 total_us=", elapsed / 1000,
          " atlas_us=", completedRenderTrace.atlasNanos / 1000,
          " atlas_calls=", completedRenderTrace.atlasCalls,
          " atlas_cached=", completedRenderTrace.atlasCached,
          " atlas_real=", completedRenderTrace.atlasReal,
          " text=\"", preview, "\"");
      }
    }

    if (profile && validSemanticKey) {
      const auto* renderer = reinterpret_cast<const BYTE*>(a);
      uintptr_t outputAddress = 0;
      std::memcpy(&outputAddress, renderer + 0x1a0, sizeof(outputAddress));
      const auto* output = reinterpret_cast<const BYTE*>(outputAddress);
      RenderTextOutputSignature signature;
      uintptr_t pixelsAddress = 0;
      if (output) {
        std::memcpy(&signature.width, output, sizeof(signature.width));
        std::memcpy(&signature.height, output + 4, sizeof(signature.height));
        std::memcpy(&pixelsAddress, output + 8, sizeof(pixelsAddress));
        std::memcpy(signature.metrics.data(), output + 0x10,
          sizeof(signature.metrics));
      }
      signature.result = result;
      signature.byteCount = signature.width > 0 && signature.height > 0
        ? uint64_t(uint32_t(signature.width)) * uint32_t(signature.height) : 0;
      if (pixelsAddress && signature.byteCount &&
          signature.byteCount <= 16 * 1024 * 1024) {
        if (activeTextRecordType == 19 || activeTextRecordType == 20) {
          const size_t bucket = size_t(activeTextRecordType - 19);
          deepMenu.renderTextRecordOutputBytes[bucket].fetch_add(
            signature.byteCount, std::memory_order_relaxed);
          auto& maximum = deepMenu.renderTextRecordMaxOutputBytes[bucket];
          uint64_t previous = maximum.load(std::memory_order_relaxed);
          while (previous < signature.byteCount &&
                 !maximum.compare_exchange_weak(previous, signature.byteCount,
                   std::memory_order_relaxed)) { }
        }
        const auto* bytes = reinterpret_cast<const uint8_t*>(pixelsAddress);
        uint64_t hash = 0xcbf29ce484222325ULL;
        for (uint64_t i = 0; i < signature.byteCount; i++) {
          hash ^= bytes[i];
          hash *= 0x100000001b3ULL;
        }
        signature.byteHash = hash;
        std::lock_guard lock(renderBitmapMutex);
        const auto [found, inserted] = renderOutputSignatures.emplace(
          cacheKey, signature);
        if (!inserted) {
          if (found->second == signature)
            ++renderOutputRepeatMatches;
          else
            ++renderOutputRepeatConflicts;
        }
      } else {
        std::lock_guard lock(renderBitmapMutex);
        ++renderOutputInvalidSamples;
      }
    }

    if (validCacheKey && result == 0) {
      const auto* renderer = reinterpret_cast<const BYTE*>(a);
      uintptr_t outputAddress = 0;
      std::memcpy(&outputAddress, renderer + 0x1a0, sizeof(outputAddress));
      const auto* output = reinterpret_cast<const BYTE*>(outputAddress);
      RenderTextBitmap bitmap;
      uintptr_t pixelsAddress = 0;
      if (output) {
        std::memcpy(&bitmap.width, output, sizeof(bitmap.width));
        std::memcpy(&bitmap.height, output + 4, sizeof(bitmap.height));
        std::memcpy(&pixelsAddress, output + 8, sizeof(pixelsAddress));
        std::memcpy(bitmap.metrics.data(), output + 0x10,
          sizeof(bitmap.metrics));
      }
      const uint64_t size = bitmap.width > 0 && bitmap.height > 0
        ? uint64_t(uint32_t(bitmap.width)) * uint32_t(bitmap.height) : 0;
      if (pixelsAddress && size && size <= 16 * 1024 * 1024) {
        bitmap.result = result;
        bitmap.bytes.resize(size_t(size));
        std::memcpy(bitmap.bytes.data(),
          reinterpret_cast<const void*>(pixelsAddress), size_t(size));
        std::lock_guard lock(renderBitmapMutex);
        renderBitmapCache.emplace(std::move(cacheKey), std::move(bitmap));
      }
    }
  }

  if (profile) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - started).count();
    deepMenu.renderTextCalls.fetch_add(1, std::memory_order_relaxed);
    deepMenu.renderTextNanos.fetch_add(
      uint64_t(elapsed), std::memory_order_relaxed);
    deepMenu.renderTextBytes.fetch_add(length, std::memory_order_relaxed);
    if (activeTextRecordType == 19 || activeTextRecordType == 20) {
      const size_t bucket = size_t(activeTextRecordType - 19);
      deepMenu.renderTextRecordCalls[bucket].fetch_add(
        1, std::memory_order_relaxed);
      deepMenu.renderTextRecordNanos[bucket].fetch_add(
        uint64_t(elapsed), std::memory_order_relaxed);
      deepMenu.renderTextRecordBytes[bucket].fetch_add(
        length, std::memory_order_relaxed);
      renderTextExactKeysByRecordType[bucket].insert(exactKey);
      if (validSemanticKey) {
        std::lock_guard lock(renderBitmapMutex);
        auto& timing = renderKeyTimings[cacheKey];
        timing.recordType = activeTextRecordType;
        ++timing.calls;
        timing.totalNanos += uint64_t(elapsed);
        timing.minimumNanos = std::min(
          timing.minimumNanos, uint64_t(elapsed));
        timing.maximumNanos = std::max(
          timing.maximumNanos, uint64_t(elapsed));
      }
    }
    renderTextKeys.insert(key);
    renderTextExactKeys.insert(exactKey);
    if (b)
      renderTextPointers.insert(b);
    if (a)
      renderTextRenderers.insert(a);
  }
  return result;
}

uintptr_t cachedAtlasLock(uintptr_t texture, uintptr_t output,
                          uintptr_t level, uintptr_t face) {
  const auto traceStarted = activeRenderTrace.active
    ? std::chrono::steady_clock::now()
    : std::chrono::steady_clock::time_point{};
  const auto finishTrace = [&](bool cached) {
    if (traceStarted == std::chrono::steady_clock::time_point{})
      return;
    const uint64_t elapsed = uint64_t(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - traceStarted).count());
    activeRenderTrace.atlasNanos += elapsed;
    ++activeRenderTrace.atlasCalls;
    if (cached)
      ++activeRenderTrace.atlasCached;
    else
      ++activeRenderTrace.atlasReal;
  };
  uint16_t width = 0;
  uint16_t height = 0;
  if (texture) {
    const auto* bytes = reinterpret_cast<const BYTE*>(texture);
    std::memcpy(&width, bytes + 0x40, sizeof(width));
    std::memcpy(&height, bytes + 0x42, sizeof(height));
  }

  const bool inCandidateScope = renderTextDepth && output && width == 512 &&
    height == 512 && atlasCacheActive.load(std::memory_order_acquire);
  const bool candidate = inCandidateScope && atlasCacheEnabled();
  if (candidate) {
    std::lock_guard lock(atlasMutex);
    const auto found = atlasReads.find(texture);
    if (found != atlasReads.end() && !found->second.bytes.empty()) {
      if (menuStatsEnabled())
        atlasCacheHits.fetch_add(1, std::memory_order_relaxed);
      *reinterpret_cast<void**>(output) = found->second.bytes.data();
      syntheticAtlasLocks.push_back(texture);
      finishTrace(true);
      return found->second.pitch;
    }
  }

  if (candidate && menuStatsEnabled())
    atlasRealReads.fetch_add(1, std::memory_order_relaxed);
  const uintptr_t pitch = originalAtlasLock(texture, output, level, face);
  if (candidate && pitch && pitch <= 16384) {
    const void* mapped = *reinterpret_cast<void* const*>(output);
    const size_t size = size_t(pitch) * height;
    if (mapped && size <= 8 * 1024 * 1024) {
      AtlasRead entry;
      entry.pitch = uint32_t(pitch);
      entry.bytes.resize(size);
      std::memcpy(entry.bytes.data(), mapped, size);
      std::lock_guard lock(atlasMutex);
      atlasReads.emplace(texture, std::move(entry));
    }
  }
  if (candidate && pitch)
    realCandidateAtlasLocks.push_back(texture);
  finishTrace(false);
  return pitch;
}

uintptr_t cachedAtlasUnlock(uintptr_t texture, uintptr_t a,
                            uintptr_t b, uintptr_t c) {
  if (atlasCacheActive.load(std::memory_order_acquire) &&
      !syntheticAtlasLocks.empty() &&
      syntheticAtlasLocks.back() == texture) {
    syntheticAtlasLocks.pop_back();
    return 0;
  }
  if (!realCandidateAtlasLocks.empty() &&
      realCandidateAtlasLocks.back() == texture) {
    realCandidateAtlasLocks.pop_back();
  } else if (frameAtlasCacheEnabled() && texture) {
    std::lock_guard lock(atlasMutex);
    atlasReads.erase(texture);
  }
  return originalAtlasUnlock(texture, a, b, c);
}

void writeAbsoluteJump(BYTE* destination, const void* target) {
  destination[0] = 0xff;
  destination[1] = 0x25;
  std::memset(destination + 2, 0, 4);
  const uintptr_t address = reinterpret_cast<uintptr_t>(target);
  std::memcpy(destination + 6, &address, sizeof(address));
}

bool installDetour(BYTE* target, const void* replacement,
                   size_t patchSize, void** original) {
  if (patchSize < 14)
    return false;
  auto* trampoline = static_cast<BYTE*>(VirtualAlloc(
    nullptr, patchSize + 14, MEM_COMMIT | MEM_RESERVE,
    PAGE_EXECUTE_READWRITE));
  if (!trampoline)
    return false;
  std::memcpy(trampoline, target, patchSize);
  writeAbsoluteJump(trampoline + patchSize, target + patchSize);
  FlushInstructionCache(GetCurrentProcess(), trampoline, patchSize + 14);
  *original = trampoline;

  DWORD oldProtection = 0;
  if (!VirtualProtect(target, patchSize, PAGE_EXECUTE_READWRITE, &oldProtection)) {
    VirtualFree(trampoline, 0, MEM_RELEASE);
    *original = nullptr;
    return false;
  }
  writeAbsoluteJump(target, replacement);
  std::memset(target + 14, 0x90, patchSize - 14);
  FlushInstructionCache(GetCurrentProcess(), target, patchSize);
  DWORD ignored = 0;
  VirtualProtect(target, patchSize, oldProtection, &ignored);
  return true;
}

template <size_t N>
bool matches(const BYTE* target, const std::array<BYTE, N>& expected) {
  return !std::memcmp(target, expected.data(), expected.size());
}

bool installMinHookDetour(BYTE* target, const void* replacement,
                          void** original) {
  const MH_STATUS created = MH_CreateHook(
    target, const_cast<void*>(replacement), original);
  if (created != MH_OK)
    return false;
  return MH_EnableHook(target) == MH_OK;
}

bool installAtlasCache(BYTE* base, const Game& game) {
  if (!atlasCacheEnabled() || game.atlasVariant == AtlasNone)
    return false;

  auto* queue = base + game.queueDrainRva;
  auto* render = base + game.renderTextRva;
  auto* lock = base + game.atlasLockRva;
  auto* unlock = base + game.atlasUnlockRva;
  const std::array<BYTE, 16> roronaQueueExpected = {
    0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0x88,
  };
  const std::array<BYTE, 16> laterQueueExpected = {
    0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0x98,
  };
  const std::array<BYTE, 16> totoriQueueExpected = {
    0x48, 0x8b, 0xc4, 0x55, 0x41, 0x54, 0x41, 0x55,
    0x41, 0x56, 0x41, 0x57, 0x48, 0x8d, 0x68, 0xb8,
  };
  const std::array<BYTE, 15> renderExpected = {
    0x48, 0x8b, 0xc4, 0x48, 0x89, 0x50, 0x10, 0x53,
    0x48, 0x81, 0xec, 0x90, 0x00, 0x00, 0x00,
  };
  const std::array<BYTE, 15> lockExpected = {
    0x48, 0x83, 0xec, 0x38, 0x44, 0x89, 0x4c, 0x24,
    0x20, 0x45, 0x8b, 0xc8, 0x45, 0x33, 0xc0,
  };
  const std::array<BYTE, 15> roronaUnlockExpected = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
  };
  const std::array<BYTE, 14> laterUnlockExpected = {
    0x44, 0x8b, 0xc2, 0x33, 0xd2, 0xe9, 0x06, 0x00,
    0x00, 0x00, 0xcc, 0xcc, 0xcc, 0xcc,
  };
  const bool signaturesMatch = game.atlasVariant == AtlasRorona
    ? matches(queue, roronaQueueExpected) &&
      matches(unlock, roronaUnlockExpected)
    : (game.atlasVariant == AtlasTotori
      ? matches(queue, totoriQueueExpected)
      : matches(queue, laterQueueExpected)) &&
      matches(unlock, laterUnlockExpected);
  if (!signaturesMatch || !matches(render, renderExpected) ||
      !matches(lock, lockExpected))
    return false;

  /* This order keeps every partial-install outcome inert: synthetic locks
   * require the render hook and an active drain, installed last. */
  if (!installMinHookDetour(unlock,
      reinterpret_cast<void*>(&cachedAtlasUnlock),
      reinterpret_cast<void**>(&originalAtlasUnlock)))
    return false;
  if (!installMinHookDetour(lock, reinterpret_cast<void*>(&cachedAtlasLock),
      reinterpret_cast<void**>(&originalAtlasLock)))
    return false;
  if (!installMinHookDetour(render,
      reinterpret_cast<void*>(&cachedRenderText),
      reinterpret_cast<void**>(&originalRenderText)))
    return false;
  return installMinHookDetour(queue,
    reinterpret_cast<void*>(&cachedQueueDrain),
    reinterpret_cast<void**>(&originalQueueDrain));
}

bool installDeepMenuStats(BYTE* base, const Game& game) {
  if (!deepMenuStatsEnabled() || game.atlasVariant != AtlasRorona ||
      game.exeBuild != BuildEnglish)
    return false;

  auto* resource = base + 0x167840;
  auto* layout = base + 0x166540;
  auto* layoutLookup = base + 0x09d6f0;
  auto* layoutCreate = base + 0x167a00;
  auto* layoutApply = base + 0x169160;
  auto* layoutBuildCore = base + 0x52ce90;
  auto* layoutEntryInit = base + 0x169f00;
  auto* layoutAcquire = base + 0x163eb0;
  auto* layoutApplyCore = base + 0x46f870;
  auto* layoutObjectE8 = base + 0x471b40;
  auto* layoutObjectF0 = base + 0x4b7210;
  auto* layoutObjectC8 = base + 0x4b7b20;
  auto* layoutValidateC8 = base + 0x471dc0;
  auto* layoutValidateF0 = base + 0x4b72c0;
  auto* layoutRebuild = base + 0x4af8e0;
  auto* layoutAllocate = base + 0x506e60;
  auto* layoutTemplate = base + 0x52cb40;
  auto* record = base + 0x0a5f40;
  auto* node = base + 0x0a2ef0;
  const std::array<BYTE, 15> resourceExpected = {
    0x48, 0x8b, 0xc4, 0x57, 0x48, 0x81, 0xec, 0xd0,
    0x00, 0x00, 0x00, 0x48, 0xc7, 0x44, 0x24,
  };
  const std::array<BYTE, 15> layoutExpected = {
    0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x00,
  };
  const std::array<BYTE, 15> nodeExpected = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x74,
    0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x60,
  };
  const std::array<BYTE, 15> recordExpected = {
    0x44, 0x89, 0x44, 0x24, 0x18, 0x56, 0x57, 0x41,
    0x56, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7,
  };
  const std::array<BYTE, 15> layoutLookupExpected = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
  };
  const std::array<BYTE, 15> layoutCreateExpected = {
    0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48,
    0x81, 0xec, 0x20, 0x01, 0x00, 0x00, 0x48,
  };
  const std::array<BYTE, 15> layoutApplyExpected = {
    0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48,
    0x81, 0xec, 0xd0, 0x00, 0x00, 0x00, 0x48,
  };
  const std::array<BYTE, 15> layoutBuildCoreExpected = {
    0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81,
  };
  const std::array<BYTE, 15> layoutEntryInitExpected = {
    0x85, 0xd2, 0x0f, 0x88, 0x85, 0x00, 0x00, 0x00,
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48,
  };
  const std::array<BYTE, 15> layoutAcquireExpected = {
    0x48, 0x89, 0x5c, 0x24, 0x10, 0x48, 0x89, 0x6c,
    0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20,
  };
  const std::array<BYTE, 15> layoutApplyCoreExpected = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
  };
  const std::array<BYTE, 15> layoutObjectE8Expected = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7,
    0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff,
  };
  const std::array<BYTE, 15> layoutObjectF0Expected = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8d,
    0x0d, 0xb3, 0x91, 0xbe, 0x00, 0x48, 0x8b,
  };
  const std::array<BYTE, 15> layoutObjectC8Expected = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
  };
  const std::array<BYTE, 15> layoutValidateC8Expected = {
    0x41, 0x56, 0x48, 0x83, 0xec, 0x40, 0x48, 0xc7,
    0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff,
  };
  const std::array<BYTE, 15> layoutValidateF0Expected = {
    0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x83, 0xec, 0x40, 0x48,
  };
  const std::array<BYTE, 15> layoutRebuildExpected = {
    0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x81, 0xec, 0xd0, 0x00,
  };
  const std::array<BYTE, 15> layoutAllocateExpected = {
    0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
    0x24, 0x10, 0x57, 0x48, 0x83, 0xec, 0x20,
  };
  const std::array<BYTE, 15> layoutTemplateExpected = {
    0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81,
  };
  if (!matches(resource, resourceExpected) ||
      !matches(layout, layoutExpected) || !matches(node, nodeExpected) ||
      !matches(record, recordExpected) ||
      !matches(layoutLookup, layoutLookupExpected) ||
      !matches(layoutCreate, layoutCreateExpected) ||
      !matches(layoutApply, layoutApplyExpected) ||
      !matches(layoutBuildCore, layoutBuildCoreExpected) ||
      !matches(layoutEntryInit, layoutEntryInitExpected) ||
      !matches(layoutAcquire, layoutAcquireExpected) ||
      !matches(layoutApplyCore, layoutApplyCoreExpected) ||
      !matches(layoutObjectE8, layoutObjectE8Expected) ||
      !matches(layoutObjectF0, layoutObjectF0Expected) ||
      !matches(layoutObjectC8, layoutObjectC8Expected) ||
      !matches(layoutValidateC8, layoutValidateC8Expected) ||
      !matches(layoutValidateF0, layoutValidateF0Expected) ||
      !matches(layoutRebuild, layoutRebuildExpected) ||
      !matches(layoutAllocate, layoutAllocateExpected) ||
      !matches(layoutTemplate, layoutTemplateExpected))
    return false;

  /* Descendant hooks remain inert until the node hook, installed last,
   * establishes the thread-local scope. */
  if (!installMinHookDetour(resource,
      reinterpret_cast<void*>(&timedNodeResource),
      reinterpret_cast<void**>(&originalNodeResource)))
    return false;
  if (!installMinHookDetour(layout,
      reinterpret_cast<void*>(&timedNodeLayout),
      reinterpret_cast<void**>(&originalNodeLayout)))
    return false;
  if (!installMinHookDetour(layoutLookup,
      reinterpret_cast<void*>(&timedLayoutLookup),
      reinterpret_cast<void**>(&originalLayoutLookup)))
    return false;
  if (!installMinHookDetour(layoutCreate,
      reinterpret_cast<void*>(&timedLayoutCreate),
      reinterpret_cast<void**>(&originalLayoutCreate)))
    return false;
  if (!installMinHookDetour(layoutApply,
      reinterpret_cast<void*>(&timedLayoutApply),
      reinterpret_cast<void**>(&originalLayoutApply)))
    return false;
  if (!installMinHookDetour(layoutBuildCore,
      reinterpret_cast<void*>(&timedLayoutBuildCore),
      reinterpret_cast<void**>(&originalLayoutBuildCore)))
    return false;
  if (!installMinHookDetour(layoutEntryInit,
      reinterpret_cast<void*>(&timedLayoutEntryInit),
      reinterpret_cast<void**>(&originalLayoutEntryInit)))
    return false;
  if (!installMinHookDetour(layoutAcquire,
      reinterpret_cast<void*>(&timedLayoutAcquire),
      reinterpret_cast<void**>(&originalLayoutAcquire)))
    return false;
  if (!installMinHookDetour(layoutApplyCore,
      reinterpret_cast<void*>(&timedLayoutApplyCore),
      reinterpret_cast<void**>(&originalLayoutApplyCore)))
    return false;
  if (!installMinHookDetour(layoutObjectE8,
      reinterpret_cast<void*>(&timedLayoutObjectE8),
      reinterpret_cast<void**>(&originalLayoutObjectE8)))
    return false;
  if (!installMinHookDetour(layoutObjectF0,
      reinterpret_cast<void*>(&timedLayoutObjectF0),
      reinterpret_cast<void**>(&originalLayoutObjectF0)))
    return false;
  if (!installMinHookDetour(layoutObjectC8,
      reinterpret_cast<void*>(&timedLayoutObjectC8),
      reinterpret_cast<void**>(&originalLayoutObjectC8)))
    return false;
  if (!installMinHookDetour(layoutValidateC8,
      reinterpret_cast<void*>(&timedLayoutValidateC8),
      reinterpret_cast<void**>(&originalLayoutValidateC8)))
    return false;
  if (!installMinHookDetour(layoutValidateF0,
      reinterpret_cast<void*>(&timedLayoutValidateF0),
      reinterpret_cast<void**>(&originalLayoutValidateF0)))
    return false;
  if (!installMinHookDetour(layoutRebuild,
      reinterpret_cast<void*>(&timedLayoutRebuild),
      reinterpret_cast<void**>(&originalLayoutRebuild)))
    return false;
  if (!installMinHookDetour(layoutAllocate,
      reinterpret_cast<void*>(&timedLayoutAllocate),
      reinterpret_cast<void**>(&originalLayoutAllocate)))
    return false;
  if (!installMinHookDetour(layoutTemplate,
      reinterpret_cast<void*>(&timedLayoutTemplate),
      reinterpret_cast<void**>(&originalLayoutTemplate)))
    return false;
  if (!installMinHookDetour(record,
      reinterpret_cast<void*>(&timedRecord),
      reinterpret_cast<void**>(&originalRecord)))
    return false;
  return installMinHookDetour(node, reinterpret_cast<void*>(&timedNodeInit),
    reinterpret_cast<void**>(&originalNodeInit));
}

bool installTextBitmapAllocator(BYTE* base, const Game& game) {
  if (!textBitmapCacheEnabled() || game.atlasVariant != AtlasRorona ||
      game.exeBuild != BuildEnglish)
    return false;
  auto* allocate = base + 0x262e90;
  auto* release = base + 0x262d60;
  const std::array<BYTE, 10> allocateExpected = {
    0xba, 0x10, 0x00, 0x00, 0x00, 0xe9, 0x36, 0xff, 0xff, 0xff,
  };
  const std::array<BYTE, 15> releaseExpected = {
    0x48, 0x85, 0xc9, 0x74, 0x61, 0x53, 0x48, 0x83,
    0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0xae,
  };
  if (!matches(allocate, allocateExpected) ||
      !matches(release, releaseExpected))
    return false;
  gameAlloc = reinterpret_cast<GameAllocProc>(allocate);
  gameFree = reinterpret_cast<GameFreeProc>(release);
  return true;
}

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
// check on RoronaBuildAddrs::managerSlot.
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

// The engine's universal "set shader float uniform by name": prologue at 0xc61e0
// is  push rbx/rsi/rdi; sub rsp,0xc0; movaps [rsp+0xb0],xmm6  and it consumes
// rcx=object, rdx=name, xmm2=value, r9=arg3 (verified by disasm). Match that
// signature exactly so the pass-through can't corrupt the call.
using SetUniformProc = void* (*)(void*, const char*, float, void*);
SetUniformProc originalSetUniform = nullptr;

void* tracedSetUniform(void* obj, const char* name, float value, void* arg3) {
  if (uniformTraceEnabled() && name) {
    const char* state = currentBattleState();
    if (state) {
      // Log first sight of each name and thereafter only when its value has
      // CHANGED (>1e-4) and at most ~4/sec/name — so a step at the cut-in shows
      // without a smoothly-animating uniform flooding the log.
      static std::mutex m;
      static std::unordered_map<const char*, std::pair<float, uint64_t>> seen;
      const uint64_t tick = GetTickCount64();
      bool doLog = false;
      {
        std::lock_guard<std::mutex> lock(m);
        auto it = seen.find(name);
        const float d = it == seen.end() ? 1.0f : value - it->second.first;
        const float ad = d < 0 ? -d : d;
        if (it == seen.end()) {
          seen[name] = {value, tick};
          doLog = true;
        } else if (ad > 1e-4f && tick - it->second.second > 250) {
          it->second = {value, tick};
          doLog = true;
        }
      }
      if (doLog)
        atfix::log("UNIFORM_SET name=", name, " val=", value,
          " cine=", atfix::arlandInCinematicBattle(), " state=", state);
    }
  }
  return originalSetUniform(obj, name, value, arg3);
}

bool installUniformTrace(BYTE* base, const Game& game) {
  if (!uniformTraceEnabled() || game.atlasVariant != AtlasRorona ||
      game.exeBuild != BuildEnglish)
    return false;
  auto* target = base + 0xc61e0;
  const std::array<BYTE, 16> expected = {
    0x40, 0x53, 0x56, 0x57, 0x48, 0x81, 0xec, 0xc0,
    0x00, 0x00, 0x00, 0x0f, 0x29, 0xb4, 0x24, 0xb0,
  };
  if (!matches(target, expected))
    return false;
  return installMinHookDetour(target,
    reinterpret_cast<void*>(&tracedSetUniform),
    reinterpret_cast<void**>(&originalSetUniform));
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

void detectAndInstallGameHooks() {
  HMODULE module = GetModuleHandleW(nullptr);
  char imagePath[MAX_PATH] = {};
  if (!module || !GetModuleFileNameA(module, imagePath, sizeof(imagePath)))
    return;
  const DWORD textSize = textSectionSize(module);
  for (const auto& game : games) {
    if (_stricmp(baseName(imagePath), game.executable) || textSize != game.textSize)
      continue;
    supportedGame = true;
    frameAtlasCacheDefault.store(
      game.atlasVariant == AtlasRorona, std::memory_order_relaxed);
    if (game.atlasVariant == AtlasRorona) {
      g_roronaAddrs = game.exeBuild == BuildMultilingual
        ? &kRoronaAddrsMulti : &kRoronaAddrsEn;
      g_battleStates = game.exeBuild == BuildMultilingual
        ? kBattleStatesMulti : kBattleStatesEn;
      g_battleStateCount = game.exeBuild == BuildMultilingual
        ? std::size(kBattleStatesMulti) : std::size(kBattleStatesEn);
    }
    atfix::log("Recognized menu-fix executable ", game.executable,
      game.exeBuild == BuildMultilingual ? " (multilingual build)"
                                         : " (English build)");
    const char* enabled = std::getenv("ARLAND_MENU_FIX");
    if (enabled && enabled[0] == '0')
      return;
    auto* target = reinterpret_cast<BYTE*>(module) + game.pathCheckRva;
    const std::array<BYTE, 18> expected = {
      0x40, game.pushedRegister, 0x48, 0x81, 0xec, 0xc0, 0x00, 0x00,
      0x00, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff,
    };
    bool pathInstalled = false;
    if (matches(target, expected))
      pathInstalled = installDetour(target, reinterpret_cast<void*>(&cachedPathCheck),
        expected.size(), reinterpret_cast<void**>(&originalPathCheck));
    const bool atlasInstalled = installAtlasCache(
      reinterpret_cast<BYTE*>(module), game);
    gameBase = reinterpret_cast<BYTE*>(module);
    const bool textBitmapAllocatorInstalled =
      installTextBitmapAllocator(gameBase, game);
    const bool deepStatsInstalled = atlasInstalled
      ? installDeepMenuStats(gameBase, game) : false;
    const bool shadowLayerTraceInstalled =
      installShadowLayerTrace(gameBase, game);
    const bool shadowConstructorTraceInstalled =
      installShadowConstructorTrace(gameBase, game);
    const bool shadowMappingTraceInstalled =
      installShadowMappingTrace(gameBase, game);
    const bool cutinFlagTraceInstalled =
      installCutinFlagTrace(gameBase, game);
    if (cutinFlagTraceEnabled())
      atfix::log("Cutin flag trace installed=", cutinFlagTraceInstalled);
    const bool uniformTraceInstalled =
      installUniformTrace(gameBase, game);
    if (uniformTraceEnabled())
      atfix::log("Uniform trace installed=", uniformTraceInstalled);
    atfix::log("Menu hooks pssg=", pathInstalled,
      " atlas=", atlasInstalled,
      " frame_atlas_cache=", frameAtlasCacheEnabled(),
      " text_bitmap_cache=", textBitmapCacheEnabled(),
      " text_bitmap_allocator=", textBitmapAllocatorInstalled,
      " stats=", menuStatsEnabled(),
      " deep_stats=", deepStatsInstalled,
      " shadow_layers=", shadowLayerTraceInstalled,
      " shadow_constructors=", shadowConstructorTraceInstalled,
      " shadow_mapping=", shadowMappingTraceInstalled);
    return;
  }
}

} // namespace

// Log the active scene identity when it changes. The persistent scene manager
// ([0x1410c73c8]) holds the scene container at +0x10; an attack cut-in is
// suspected to swap in a distinct scene/camera object there. Logging on change
// catches a brief cut-in the coarse per-120-frame monitor would miss. Reads
// only fields the game populates; every access is VirtualQuery-guarded.
void sceneIdentityTick() {
  if (!sceneTraceEnabled() || !gameBase || !g_roronaAddrs)
    return;
  const uintptr_t managerSlot =
    reinterpret_cast<uintptr_t>(gameBase) + g_roronaAddrs->managerSlot;
  if (!readableRange(managerSlot, sizeof(uintptr_t)))
    return;
  const uintptr_t manager = *reinterpret_cast<const uintptr_t*>(managerSlot);
  const uintptr_t helper = readableRange(manager, 0xa00)
    ? *reinterpret_cast<const uintptr_t*>(manager + 0x9d0) : 0;

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
  if (slot && (!readableRange(slot, 8) ||
      !battleStateName(*reinterpret_cast<const uintptr_t*>(
        *reinterpret_cast<const uintptr_t*>(slot))))) {
    slot = 0;
    g_battleStateSlot.store(0, std::memory_order_release);
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
  const uintptr_t stateObj = *reinterpret_cast<const uintptr_t*>(slot);
  const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(stateObj);
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
  if (!gameMode || !readableRange(gameMode + 0x658, 0x10))
    return;
  const uintptr_t begin = *reinterpret_cast<const uintptr_t*>(gameMode + 0x658);
  const uintptr_t end = *reinterpret_cast<const uintptr_t*>(gameMode + 0x658 + 8);
  if (!begin || end <= begin || (end - begin) > 0x200 ||
      !readableRange(begin, end - begin))
    return;
  ++snaps;
  // Dump the shadow caster registry nodes (helper+0x48) — the actual nodes the
  // shadow traversal walks; their per-node visibility gates the shadow draw.
  const uintptr_t helper = gameMode + 0x68;
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
  };
  for (const char* n : kNames)
    if (std::strcmp(name, n) == 0)
      return true;
  return false;
}

// Exposed to the D3D layer (sync_fix) so draws can be tagged cut-in vs overview.
namespace atfix {
bool arlandInCinematicBattle() {
  return isCinematicState(currentBattleState());
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
}

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
          const char* fc = (v && !(v & 7) && readableRange(v, 8))
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

// Is the battle game-mode still a live battle (party vector at +0x658 still
// holds BtlChara objects)? Used to detect battle exit so we can un-publish the
// battle helper before the field renders through a freed pointer.
bool battleGameModeLive(uintptr_t gameMode) {
  if (!gameMode || !readableRange(gameMode + 0x658, 0x10))
    return false;
  const uintptr_t begin =
    *reinterpret_cast<const uintptr_t*>(gameMode + 0x658);
  const uintptr_t end =
    *reinterpret_cast<const uintptr_t*>(gameMode + 0x658 + 8);
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

  // Self-healing restore: once we've published (saved != 0), watch the battle
  // game-mode. When it stops looking live for several consecutive frames, the
  // battle is over — put the field helper back and stand down.
  if (g_savedGlobalHelper.load(std::memory_order_acquire)) {
    if (battleGameModeLive(gameMode)) {
      g_battleDeadFrames.store(0, std::memory_order_release);
    } else if (g_battleDeadFrames.fetch_add(1, std::memory_order_acq_rel) >= 20) {
      restorePublishedHelper("gamemode_dead");
      g_battleActive.store(false, std::memory_order_release);
      g_battleContainerFound.store(false, std::memory_order_release);
      g_battleRegistered.store(false, std::memory_order_release);
      g_battleDeadFrames.store(0, std::memory_order_release);
      return;
    }
  }

  if (!g_battleActive.load(std::memory_order_acquire))
    return;
  const uint64_t tick = g_battleTickCounter.fetch_add(
    1, std::memory_order_relaxed);
  const uintptr_t scene = g_battleScene.load(std::memory_order_acquire);

  if (!g_battleContainerFound.load(std::memory_order_acquire) &&
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

namespace arland {

bool initializeGameHooks() {
  std::call_once(initialization, detectAndInstallGameHooks);
  return supportedGame;
}

bool frameAtlasCacheEnabled() {
  return ::frameAtlasCacheEnabled();
}

// True when the recognized executable is a Rorona build with the battle-shadow
// restore enabled; the per-frame battle machinery then needs the Present hook
// regardless of the frame-atlas-cache setting.
bool battleShadowRestoreActive() {
  return supportedGame && g_roronaAddrs && battleShadowRestoreEnabled();
}

void traceMenuPresent(uint64_t durationMicros, uint64_t intervalMicros) {
  sceneIdentityTick();
  trackBattleStateTick();
  battleShadowFrameTick();
  cutinReregisterTick();
  // Manual correlation marker: press F7/F8/F9 while a cut-in is on screen. Uses
  // the currently-down bit (0x8000) with our own edge detection — Wine does not
  // reliably implement GetAsyncKeyState's "recently pressed" low bit. The ms
  // timestamp matches against the ms on each SHADOW window line.
  static bool markKeyWasDown[3] = {false, false, false};
  const int markKeys[3] = {VK_F7, VK_F8, VK_F9};
  for (int i = 0; i < 3; ++i) {
    const bool down = (GetAsyncKeyState(markKeys[i]) & 0x8000) != 0;
    if (down && !markKeyWasDown[i])
      atfix::log("USER_MARK key=F", 7 + i, " ms=", GetTickCount64(),
        " battle_active=", g_battleActive.load(std::memory_order_acquire));
    markKeyWasDown[i] = down;
  }
  if (frameAtlasCacheEnabled()) {
    std::lock_guard lock(atlasMutex);
    atlasReads.clear();
    atlasCacheActive.store(true, std::memory_order_release);
  }
  uint32_t budget = transitionPresentBudget.load(std::memory_order_acquire);
  while (budget && !transitionPresentBudget.compare_exchange_weak(
      budget, budget - 1, std::memory_order_acq_rel,
      std::memory_order_acquire)) { }
  if (!budget)
    return;
  const uint32_t index = transitionPresentIndex.fetch_add(
    1, std::memory_order_relaxed);
  atfix::log("TRANSITION present id=",
    transitionSequence.load(std::memory_order_relaxed),
    " frame=", index,
    " interval_us=", intervalMicros,
    " present_us=", durationMicros,
    " drain_us=", transitionDrainMicros.load(std::memory_order_relaxed),
    " thread=", GetCurrentThreadId());
}

} // namespace arland
