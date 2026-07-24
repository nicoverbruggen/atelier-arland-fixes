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
#include "font_hires.h"
#include "game.h"
#include "log.h"
#include "mem.h"   // readableRange + tryRead (guarded game-memory reads)
#include "hook_util.h"   // Game, atlas/build enums, matches, installDetour
#include "menu_internal.h"   // gameBase, supportedGame, battle entry points

namespace atfix {
extern Log log;
bool arlandInCinematicBattle();   // defined later in this TU
bool arlandConfigBool(const char* section, const char* key, bool def);  // sync_fix.cpp
// Shared with battle_shadow_restore.cpp (declared in menu_internal.h).
BYTE* gameBase = nullptr;
bool supportedGame = false;
}


// ============================================================================
// menu_fix.cpp: the executable-specific menu hooks. It recognizes the running
// Arland executable and installs the menu-hitch fix and text-rendering hooks. The
// battle-shadow-restore subsystem was split into battle_shadow_restore.cpp, driven
// through the two entry points in menu_internal.h; the shared MinHook install
// helpers and the per-game Game descriptor live in hook_util.h. Sections:
//
//   1. Game address table (games[]) + build enums (English/multilingual).
//   2. Feature/trace gates (menu and deep-menu stats).
//   3. Module helpers (baseName / textSectionSize).
//   4. Menu-hitch fix: .PSSG validation cache, atlas read/frame cache, and the
//      node/record/layout construction hooks that eliminate the text-menu stalls.
//   5. Text rendering: typo fixups, the hi-res text consumer, and the Meruru
//      conversation-balloon (BUC) cross-frame text cache.
//   6. detectAndInstallGameHooks: matches the running exe and installs 1-5, plus
//      the battle subsystem via atfix::installBattleShadowRestore.
//   7. namespace arland: the public API main.cpp calls; the Present-time tick
//      drives the battle subsystem through atfix::battleFrameTick.
// ============================================================================

namespace {

using PathCheckProc = bool (*)(void*, void*);
using QueueDrainProc = void (*)(void*);
using RenderTextProc = uintptr_t (*)(
  uintptr_t, uintptr_t, uintptr_t, uintptr_t);
using SetTextProc = uintptr_t (*)(
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

// The per-game descriptor (Game), the atlas/build enums, the prologue-match
// helper and the detour installers live in hook_util.h now, shared with
// battle_shadow_restore.cpp. This TU's code is in the global anonymous namespace,
// so pull them in for unqualified use (they are declared in namespace atfix).
using atfix::Game;
using atfix::AtlasNone;
using atfix::AtlasRorona;
using atfix::AtlasTotori;
using atfix::AtlasLaterArland;
using atfix::BuildEnglish;
using atfix::BuildMultilingual;
using atfix::matches;
using atfix::writeAbsoluteJump;
using atfix::installDetour;
using atfix::installMinHookDetour;

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
SetTextProc originalSetText = nullptr;
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
std::once_flag initialization;
using atfix::gameBase;
using atfix::supportedGame;
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
// Count of live BalloonBucMode instances (Meruru's animated-portrait field
// conversations). While non-zero, the text-bitmap replay cache switches to a
// cross-frame scope: the balloon's per-frame callback pump re-runs the slow EN
// text-render path with identical text every frame, so persisting rendered
// bitmaps across queue drains turns that per-frame cost into a memcpy replay.
// Only ever raised by the BUC ctor/dtor hooks (installBucTextCacheScope).
std::atomic<uint32_t> bucBalloonCount = { 0 };

bool bucTextCacheActive() {
  return bucBalloonCount.load(std::memory_order_acquire) != 0;
}

// ARLAND_MENU_STATS heartbeat: unconditional renderText call/time counters
// (unlike deepMenu.renderTextCalls, which only counts profiled record scopes).
// Read by the per-Present heartbeat in traceMenuPresent to localize per-frame
// text-render cost outside menus (e.g. field-state slowdowns).
std::atomic<uint64_t> renderTextHeartbeatCalls = { 0 };
std::atomic<uint64_t> renderTextHeartbeatNanos = { 0 };
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

// readableRange / tryRead are the shared guarded-read primitives from mem.h.
// This TU's code lives in the global anonymous namespace, so pull them in for
// unqualified use (they are declared in namespace atfix).
using atfix::readableRange;
using atfix::tryRead;

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
  // A live BUC balloon activates the cache regardless of the env switch; the
  // drain-scoped clears are suppressed for its duration (see cachedQueueDrain)
  // and the balloon dtor clears the cache when the last balloon goes away.
  return enabled || bucTextCacheActive();
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
  if (outermost && textBitmapCacheEnabled() && !bucTextCacheActive()) {
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
    if (textBitmapCacheEnabled() && !bucTextCacheActive()) {
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

// Display-only English typo corrections, applied at render time. Only the pointer
// handed to the renderer (and the cache/re-render derived from it) changes — the
// game's own string data is untouched — so a fix shows in every UI-font mode.
// Gated per game so a match can't fire on another title's text. Extend this table
// as typos are found. Exact-match; if a target turns out to be embedded in a
// larger string, switch that entry to a substring rewrite.
struct TextFix { atfix::Title title; const char* wrong; const char* right; };
const TextFix kTextFixes[] = {
  { atfix::Title::Totori, "Synth Cateogry", "Synth Category" },
};
uintptr_t fixupText(uintptr_t b) {
  if (!b)
    return b;
  const char* s = reinterpret_cast<const char*>(b);
  const atfix::Title t = atfix::currentTitle();
  for (const TextFix& f : kTextFixes)
    if (f.title == t && std::strcmp(s, f.wrong) == 0)
      return reinterpret_cast<uintptr_t>(f.right);
  return b;
}

uintptr_t cachedRenderText(uintptr_t a, uintptr_t b,
                           uintptr_t c, uintptr_t d) {
  b = fixupText(b);   // display-only typo corrections, before caching/rendering
  const bool profile = type19Depth && deepMenuStatsEnabled();
  // The game allocators (gameAlloc/gameFree) are optional: without them a
  // replay whose target buffer is too small falls back to a real render
  // (capacity fallback below) instead of reallocating. The BUC scope renders
  // the same text into the same output object every frame, so its replays
  // never need to grow the buffer. Balloon renders can also run outside a
  // queue drain, hence bucTextCacheActive() beside atlasCacheActive.
  const bool cache = textBitmapCacheEnabled() &&
    (atlasCacheActive.load(std::memory_order_acquire) ||
     bucTextCacheActive()) &&
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

  const bool heartbeat = menuStatsEnabled();
  const auto started = profile || heartbeat
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
      if (output && gameAlloc && gameFree &&
          (!pixelsAddress || available < bitmap.bytes.size()) &&
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
    // High-resolution UI text PoC: replace the freshly-rendered low-res string
    // bitmap with a higher-resolution re-render, keeping the engine's metrics.
    // (Called unconditionally; it self-gates and one-shot-logs its state.)
    if (a && b)
      atfix::hiResTextRerender(a, reinterpret_cast<const char*>(b),
        gameAlloc, gameFree);
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
        // The BUC scope persists across frames, and the typewriter reveal
        // inserts one entry per partial string; bound the growth. A reset re-
        // renders the strings still on screen once, then they re-cache.
        if (renderBitmapCache.size() >= 512)
          renderBitmapCache.clear();
        renderBitmapCache.emplace(std::move(cacheKey), std::move(bitmap));
      }
    }
  }

  if (heartbeat && !renderTextDepth) {
    const auto heartbeatElapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - started).count();
    renderTextHeartbeatCalls.fetch_add(1, std::memory_order_relaxed);
    renderTextHeartbeatNanos.fetch_add(
      uint64_t(heartbeatElapsed), std::memory_order_relaxed);
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

// writeAbsoluteJump / installDetour / matches / installMinHookDetour live in
// hook_util.h/.cpp now (shared with battle_shadow_restore.cpp).

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

// Wraps the text consumer (renderText's caller, which builds the per-string
// texture): after it returns, restore the output object's dims so the high-res
// substitution renders at the original on-screen size (see font_hires.h).
uintptr_t STDMETHODCALLTYPE tracedSetText(
    uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
  const uintptr_t result = originalSetText(a, b, c, d);
  atfix::hiResTextRestoreDims();
  return result;
}

// Hook the text consumer (renderText's caller) for the high-resolution text
// feature, so hiResTextRestoreDims runs after it. English builds only; the three
// consumers are homologues with a byte-identical prologue (verified 2026-07-24).
bool installHiResTextConsumer(BYTE* base, const Game& game) {
  if (!atfix::hiResTextEnabled() || game.exeBuild != BuildEnglish)
    return false;
  uintptr_t consumerRva = 0;
  switch (game.atlasVariant) {
    case AtlasRorona:      consumerRva = 0x562bd0; break;
    case AtlasTotori:      consumerRva = 0x4325a0; break;
    case AtlasLaterArland: consumerRva = 0x512d80; break;   // Meruru
    default: return false;
  }
  auto* consumer = base + consumerRva;
  const std::array<BYTE, 16> consumerExpected = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x60, 0x48, 0x8b,
    0xd9, 0x48, 0x85, 0xd2, 0x75, 0x09, 0x8d, 0x42,
  };
  if (!matches(consumer, consumerExpected))
    return false;
  return installMinHookDetour(consumer, reinterpret_cast<void*>(&tracedSetText),
    reinterpret_cast<void**>(&originalSetText));
}

bool installTextBitmapAllocator(BYTE* base, const Game& game) {
  // Resolve the engine's aligned text-buffer allocator/free — the pair the
  // renderText path uses for the output object's `+8` pixel buffer — so a
  // feature can grow/replace that buffer and have the engine free it correctly.
  // English-build-only (the RVAs are per-build); needed by the text-bitmap
  // replay cache and by the high-resolution text substitution. Rorona's
  // allocate is a `mov edx,0x10; jmp` thunk; Totori/Meruru inline the alignment
  // so their allocate is a full function (same `void*(size_t)` signature).
  if (game.exeBuild != BuildEnglish ||
      (!textBitmapCacheEnabled() && !atfix::hiResTextEnabled()))
    return false;

  uintptr_t allocateRva = 0;
  uintptr_t releaseRva = 0;
  std::array<BYTE, 10> allocateExpected = {};
  std::array<BYTE, 15> releaseExpected = {};
  switch (game.atlasVariant) {
    case AtlasRorona:
      allocateRva = 0x262e90;
      releaseRva = 0x262d60;
      allocateExpected = {
        0xba, 0x10, 0x00, 0x00, 0x00, 0xe9, 0x36, 0xff, 0xff, 0xff };
      releaseExpected = {
        0x48, 0x85, 0xc9, 0x74, 0x61, 0x53, 0x48, 0x83,
        0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0xae };
      break;
    case AtlasTotori:
      allocateRva = 0x2bf6f0;
      releaseRva = 0x2bf5f0;
      allocateExpected = {
        0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x85, 0xc9, 0xbb };
      releaseExpected = {
        0x48, 0x85, 0xc9, 0x74, 0x34, 0x53, 0x48, 0x83,
        0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0x1e };
      break;
    case AtlasLaterArland:   // Meruru
      allocateRva = 0x21d220;
      releaseRva = 0x21d180;
      allocateExpected = {
        0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x85, 0xc9, 0xbb };
      releaseExpected = {
        0x48, 0x85, 0xc9, 0x74, 0x34, 0x53, 0x48, 0x83,
        0xec, 0x20, 0x48, 0x8b, 0xd9, 0xe8, 0xde };
      break;
    default:
      return false;
  }

  BYTE* allocate = base + allocateRva;
  BYTE* release = base + releaseRva;
  if (!matches(allocate, allocateExpected) ||
      !matches(release, releaseExpected))
    return false;
  gameAlloc = reinterpret_cast<GameAllocProc>(allocate);
  gameFree = reinterpret_cast<GameFreeProc>(release);
  return true;
}

// Cross-frame text-bitmap cache scope for Meruru's animated-portrait field
// conversations (BUC, "bust-up conversation"). The balloon's per-frame
// callback pump re-runs the slow EN text-render path with identical text
// every frame — the same CPU-side glyph/atlas cost the menu fix removes from
// menu rebuilds, but paid continuously — which drops the field-map framerate
// for the duration of the conversation. Hooking the BalloonBucMode ctor/dtor
// raises bucBalloonCount while any balloon is alive, which activates the
// text-bitmap replay cache (see textBitmapCacheEnabled) and suspends its
// drain-scoped clears, so each unchanged string is replayed as a memcpy.
// ARLAND_BUC_TEXT_CACHE=0 disables the scope for diagnosis.
using BucBalloonCtorProc = uintptr_t (*)(uintptr_t, uintptr_t,
                                         uintptr_t, uintptr_t);
using BucBalloonDtorProc = uintptr_t (*)(uintptr_t, uintptr_t);
BucBalloonCtorProc originalBucBalloonCtor = nullptr;
BucBalloonDtorProc originalBucBalloonDtor = nullptr;
uint64_t bucStatsHitsBase = 0;
uint64_t bucStatsMissesBase = 0;

uintptr_t scopedBucBalloonCtor(uintptr_t a, uintptr_t b,
                               uintptr_t c, uintptr_t d) {
  const uintptr_t result = originalBucBalloonCtor(a, b, c, d);
  if (bucBalloonCount.fetch_add(1, std::memory_order_acq_rel) == 0 &&
      menuStatsEnabled()) {
    bucStatsHitsBase =
      deepMenu.renderBitmapHits.load(std::memory_order_relaxed);
    bucStatsMissesBase =
      deepMenu.renderBitmapMisses.load(std::memory_order_relaxed);
  }
  return result;
}

uintptr_t scopedBucBalloonDtor(uintptr_t a, uintptr_t b) {
  // Guarded decrement: a stray dtor call with no counted ctor (partial
  // install, teardown ordering) must not wrap the counter.
  uint32_t count = bucBalloonCount.load(std::memory_order_acquire);
  while (count &&
         !bucBalloonCount.compare_exchange_weak(count, count - 1,
           std::memory_order_acq_rel)) { }
  if (count == 1) {
    if (menuStatsEnabled())
      atfix::log("BUC balloon scope end hits=",
        deepMenu.renderBitmapHits.load(std::memory_order_relaxed) -
          bucStatsHitsBase,
        " misses=",
        deepMenu.renderBitmapMisses.load(std::memory_order_relaxed) -
          bucStatsMissesBase);
    std::lock_guard lock(renderBitmapMutex);
    renderBitmapCache.clear();
  }
  return originalBucBalloonDtor(a, b);
}

bool installBucTextCacheScope(BYTE* base, const Game& game) {
  if (game.atlasVariant != AtlasLaterArland)
    return false;
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_BUC_TEXT_CACHE");
    return !value || value[0] != '0';
  }();
  if (!enabled)
    return false;
  // BalloonBucMode ctor/dtor per executable build; the multilingual RVAs were
  // homologue-matched from the English build (prologues byte-identical, only
  // the dtor's vtable-lea displacement differs).
  const bool multilingual = game.exeBuild == BuildMultilingual;
  auto* ctor = base + (multilingual ? 0x1d97a0 : 0x1e8c40);
  auto* dtor = base + (multilingual ? 0x1d9890 : 0x1e8d30);
  const std::array<BYTE, 16> ctorExpected = {
    0x48, 0x89, 0x4c, 0x24, 0x08, 0x57, 0x48, 0x83,
    0xec, 0x30, 0x48, 0xc7, 0x44, 0x24, 0x20, 0xfe,
  };
  // Includes the RIP-relative lea of the BalloonBucMode vtable (EN 0x75a758,
  // multilingual 0x73d298), pinning the hook to the right class's destructor.
  const std::array<BYTE, 24> dtorExpectedEn = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7,
    0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48,
    0x8b, 0xd9, 0x48, 0x8d, 0x05, 0x0f, 0x1a, 0x57,
  };
  const std::array<BYTE, 24> dtorExpectedMulti = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0xc7,
    0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48,
    0x8b, 0xd9, 0x48, 0x8d, 0x05, 0xef, 0x39, 0x56,
  };
  if (!matches(ctor, ctorExpected) ||
      !matches(dtor, multilingual ? dtorExpectedMulti : dtorExpectedEn))
    return false;
  // Dtor first: a stray dtor hook alone never decrements below zero, so the
  // cache scope stays inert unless the ctor hook also installed.
  if (!installMinHookDetour(dtor,
      reinterpret_cast<void*>(&scopedBucBalloonDtor),
      reinterpret_cast<void**>(&originalBucBalloonDtor)))
    return false;
  return installMinHookDetour(ctor,
    reinterpret_cast<void*>(&scopedBucBalloonCtor),
    reinterpret_cast<void**>(&originalBucBalloonCtor));
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
    installHiResTextConsumer(gameBase, game);
    // The BUC scope replays through cachedRenderText, so it needs the atlas
    // hooks; without them the ctor/dtor hooks would only count balloons.
    const bool bucTextCacheInstalled = atlasInstalled
      ? installBucTextCacheScope(gameBase, game) : false;
    const bool deepStatsInstalled = atlasInstalled
      ? installDeepMenuStats(gameBase, game) : false;
    atfix::installBattleShadowRestore(gameBase, game);
    atfix::log("Menu hooks pssg=", pathInstalled,
      " atlas=", atlasInstalled,
      " frame_atlas_cache=", frameAtlasCacheEnabled(),
      " text_bitmap_cache=", textBitmapCacheEnabled(),
      " text_bitmap_allocator=", textBitmapAllocatorInstalled,
      " buc_text_cache=", bucTextCacheInstalled,
      " stats=", menuStatsEnabled(),
      " deep_stats=", deepStatsInstalled);
    return;
  }
}

} // namespace

namespace arland {

bool initializeGameHooks() {
  std::call_once(initialization, detectAndInstallGameHooks);
  return atfix::supportedGame;
}

bool frameAtlasCacheEnabled() {
  return ::frameAtlasCacheEnabled();
}

void traceMenuPresent(uint64_t durationMicros, uint64_t intervalMicros) {
  atfix::battleFrameTick();
  // ARLAND_MENU_STATS heartbeat: every 120 Presents, log how much time went
  // into renderText and how the bitmap cache behaved, plus the mod's per-frame
  // state flags. Localizes per-frame text-render cost outside menu drains
  // (field-state slowdowns) and shows whether battle state is stuck after
  // returning to the field. Quiet while all deltas are zero.
  if (menuStatsEnabled()) {
    static uint32_t heartbeatFrames = 0;
    static uint64_t lastCalls = 0;
    static uint64_t lastNanos = 0;
    static uint64_t lastHits = 0;
    static uint64_t lastMisses = 0;
    if (++heartbeatFrames >= 120) {
      heartbeatFrames = 0;
      const uint64_t calls =
        renderTextHeartbeatCalls.load(std::memory_order_relaxed);
      const uint64_t nanos =
        renderTextHeartbeatNanos.load(std::memory_order_relaxed);
      const uint64_t hits =
        deepMenu.renderBitmapHits.load(std::memory_order_relaxed);
      const uint64_t misses =
        deepMenu.renderBitmapMisses.load(std::memory_order_relaxed);
      if (calls != lastCalls || hits != lastHits || misses != lastMisses) {
        atfix::log("TEXT heartbeat frames=120 render_calls=", calls - lastCalls,
          " render_us=", (nanos - lastNanos) / 1000,
          " bitmap_hits=", hits - lastHits,
          " bitmap_misses=", misses - lastMisses,
          " buc_balloons=", bucBalloonCount.load(std::memory_order_acquire),
          " battle_active=", atfix::g_battleActive.load(std::memory_order_acquire),
          " cinematic=", atfix::arlandInCinematicBattle());
        lastCalls = calls;
        lastNanos = nanos;
        lastHits = hits;
        lastMisses = misses;
      }
    }
  }
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
        " battle_active=", atfix::g_battleActive.load(std::memory_order_acquire));
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
