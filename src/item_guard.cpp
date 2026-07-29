// SPDX-License-Identifier: MIT
//
// Totori equipment scan guard: bounds-check the item-effect and item-trait
// index scans, which the engine leaves unchecked, so a bad index in save data
// is skipped instead of dereferenced. This fixes a reported crash on entering
// battle.
//
// The data. Totori keeps its party equipment in one global array of ten sets
// (English 0xd328d0, multilingual 0x109aa50), stride 0x18c, built by a vector
// constructor at CRT init. Each set holds three equipment slots at +0x64,
// stride 0x34, and each slot is one item record:
//
//   +0x00 dword  id            (-1 when the slot is empty)
//   +0x04 dword  id            (-1; the scan loops treat this as the sentinel)
//   +0x08 float  quality       (50.0 from the constructor)
//   +0x0c dword  traits[5]     (-1 each; indices into the trait table)
//   +0x20 dword  effects[4]    (-1 each; indices into the effect table)
//   +0x30 dword  tail
//
// The bug. Two leaf helpers scan those index arrays: the effect scan (English
// 0x25b3d0, multilingual 0x477d00) over effects[0..3], and the trait scan
// (0x25b450 / 0x477d80) over traits[0..4]. Both turn an index into a record
// address with `shl rax, 6` against a table base loaded by a RIP-relative lea,
// and both check only that the index is not negative. An index past the end of
// the table is therefore an unbounded read. That is an oversight rather than a
// design: the engine's other consumer of a trait index, at 0x250bf0, opens with
// `cmp ecx, 0xcb` and rejects anything above 203, which is exactly the trait
// table's record count. This restores that same intent to the two leaves.
//
// The trigger. Reported crash: read of 0x11dc5df5a0 at +0x25b3eb, entering
// battle. The equipped item was intact in every field the player can see --
// `id=2151 quality=51.4289 traits=[161,168,109,-1,-1]`, and index 161 resolves
// to a real effect -- but its effects array read `[-1, 2850, 234, 1114531452]`.
// That record appears verbatim at the same offset in all three save slots, and
// the following record in the save begins 0x10 bytes early, so those three
// values are another item's id, second field and quality (2850 / 234 / 59.60)
// read as effect indices. Chunk 7 supplies the mechanism: its saved container
// scan limit is 5000 although the executable constructs and serializes exactly
// 999 records. Vanilla insert/count helpers trust the saved limit, walk beyond
// that fixed array, and their 0x34 grid lands exactly at the observed embedded
// records in the equipment globals. The loader therefore receives damage that
// an earlier vanilla item operation created and the equipment writer persisted.
// The likely external seed of the round 5000 value does not make trusting it
// safe; load-time validation is required.
//
// Only the third one crashes, which is why the fault looked intermittent. 2850
// lands in zeroed .data and 234 lands in the trait table: both are wrong but
// mapped, so they read harmlessly and simply fail to match. 1114531452 is the
// bit pattern of 59.60008f, and only a float is large enough that
// `index << 6` leaves mapped memory at all.
//
// The bounds. Neither limit is asserted. The effect table's end is the trait
// table's base, which sits 0x37d0 later in both builds: 223 records. The trait
// table's end is the engine's own 0xcb compare above: 204 records, which puts
// its end at 0xc4c170, where the record blob does in fact stop. Both table
// bases are decoded at install time from each leaf's own lea rather than
// hardcoded, so a wrong address row cannot send the guard at the wrong table.
//
// Behaviour. A scan whose indices are all in range runs the original untouched,
// so an unaffected save sees no change at all. A scan with an index the table
// cannot contain is answered by a reimplementation of the same loop that skips
// that index the way a negative one is already skipped. Nothing is lost that
// existed: an out-of-range slot never named an effect, and on a damaged save it
// currently reads whatever byte pattern the index lands on, which can even
// match a query by accident. ARLAND_ITEM_GUARD=0 restores the vanilla scans for
// comparison, and ARLAND_ITEM_PROBE=1 logs every rejected index with the whole
// item record and the caller instead of only the first.
//
// On load, structurally damaged equipment, character skills, carried items and
// container items are repaired before any consumer can follow injected
// indices. The shared combat modifier builder receives a sanitized local copy
// as a second line of defence, so an item arriving through an unaudited route
// cannot index past the effect table. It also rejects a base id absent from
// Totori's fixed action-item table: vanilla uses the lookup's -1 result as a
// table index, while every caller already treats an empty modifier vector as
// valid. This check stays builder-local because equipment and materials
// legitimately use ids outside the action-item table.
// ARLAND_ITEM_SANITIZE=0 disables persistent recovery for comparison.
// ARLAND_ITEM_SAVE_TRACE=1 is a separate save-data change-tracking diagnostic:
// it shadows only the 30 saved item records, mirrors the engine's central
// equip/swap operation, and compares live memory with that model immediately
// before each save. An unexpected difference proves that some other in-memory
// route changed equipment before the serializer's flat copy.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "config.h"
#include "item_guard.h"
#include "log.h"
#include "mem.h"
#include "util.h"

namespace atfix {

extern Log log;  // main.cpp

namespace {

bool probeEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_ITEM_PROBE");
    return value && value[0] != '0';
  }();
  return enabled;
}

bool guardEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_ITEM_GUARD");
    return !value || value[0] != '0';
  }();
  return enabled;
}

bool saveTraceEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_ITEM_SAVE_TRACE");
    return value && value[0] != '0';
  }();
  return enabled;
}

bool sanitizeEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("ARLAND_ITEM_SANITIZE");
    return !value || value[0] != '0';
  }();
  return enabled;
}

// Equipment-set and item-record geometry, identical in both builds (the
// constructors at English 0x22e7a0 / 0x2599a0 are the authority).
constexpr size_t kSetStride    = 0x18c;
constexpr size_t kSetCount     = 10;
constexpr size_t kItemBase     = 0x64;
constexpr size_t kItemStride   = 0x34;
constexpr size_t kTraitOffset  = 0x0c;
constexpr size_t kTraitCount   = 5;
constexpr size_t kEffectOffset = 0x20;
constexpr size_t kEffectCount  = 4;
constexpr size_t kRecordStride = 0x40;   // both tables, `shl rax, 6`
constexpr size_t kSkillBase     = 0x100;
constexpr size_t kSkillStride   = 0x0c;
constexpr size_t kSkillCount    = 10;
constexpr size_t kCarriedCount  = 100;
constexpr size_t kContainerCount = 999;

// The trait table's record count, from the engine's own `cmp ecx, 0xcb` bound
// at 0x250bf0 (English) / 0x46d4c0 (multilingual), which is byte-identical in
// both builds. The effect table's count is derived at install time instead,
// from where the trait table begins.
constexpr int32_t kTraitRecordLimit = 204;

// Both executable action-item tables contain exactly 161 ids: the contiguous
// range 0..159 followed by 163. The lookup at English 0x25f160 / multilingual
// 0x47bb50 returns -1 for every other id, but the modifier builder uses that
// result without checking it. Do not apply this predicate to saved items
// globally: weapons, armour and materials have valid ids outside this table.
constexpr bool actionItemIdUsable(int32_t id) {
  return (id >= 0 && id <= 159) || id == 163;
}

static_assert(actionItemIdUsable(0));
static_assert(actionItemIdUsable(159));
static_assert(actionItemIdUsable(163));
static_assert(!actionItemIdUsable(-1));
static_assert(!actionItemIdUsable(160));
static_assert(!actionItemIdUsable(164));

using ScanProc = int (STDMETHODCALLTYPE*)(uintptr_t item, int32_t type);
using SaveProc = int (STDMETHODCALLTYPE*)(uintptr_t self, uintptr_t stream);
using ActualLoadProc = int (STDMETHODCALLTYPE*)(uintptr_t self);
using PreviewLoadProc = int (STDMETHODCALLTYPE*)(uintptr_t self);
using PreviewRecordProc = void (STDMETHODCALLTYPE*)(uintptr_t name);
using PreviewLabelProc = char* (STDMETHODCALLTYPE*)(uintptr_t record);
using EquipProc = uintptr_t (STDMETHODCALLTYPE*)(
  uintptr_t set, uintptr_t oldItem, uint32_t slot, uintptr_t newItem);
using RecalcProc = void (STDMETHODCALLTYPE*)(uintptr_t set);
using ItemEffectBuildProc = void (STDMETHODCALLTYPE*)(
  uintptr_t item, uintptr_t outputVector);

struct ItemGuardAddrs {
  uintptr_t effectScan;
  uintptr_t traitScan;
  uintptr_t equipArray;
  uintptr_t carriedArray;
  uintptr_t containerArray;
  uintptr_t carriedOwner;
  uintptr_t containerOwner;
  uintptr_t saveWriter;
  uintptr_t saveLoader;
  uintptr_t actualLoad;
  uintptr_t inventoryLoader;
  uintptr_t equipMutation;
  uintptr_t itemEffectBuild;
  uintptr_t skillInfo;
  uintptr_t recalc;
  uintptr_t previewLoad;
  uintptr_t previewRecord;
  uintptr_t previewRecordRet;
  uintptr_t previewLabel;
  std::array<BYTE, 16> effectExpected;
  std::array<BYTE, 16> traitExpected;
  std::array<BYTE, 16> writerExpected;
  std::array<BYTE, 16> loaderExpected;
  std::array<BYTE, 16> actualLoadExpected;
  std::array<BYTE, 16> inventoryLoaderExpected;
  std::array<BYTE, 16> equipExpected;
  std::array<BYTE, 16> itemEffectBuildExpected;
  std::array<BYTE, 16> skillExpected;
  std::array<BYTE, 16> recalcExpected;
  std::array<BYTE, 16> previewLoadExpected;
  std::array<BYTE, 16> previewRecordExpected;
  std::array<BYTE, 16> previewLabelExpected;
};

// Both leaves open with the same two instructions in both builds; only the
// RIP-relative displacement of the table lea differs, which is why these rows
// are per build rather than shared. The actual-load boundary was identified by
// a runtime stack comparison: save-list previews reach the common deserializer
// through the 0x286190/0x4a3080 tail-call path, while a selected save calls it
// directly from 0x2891d0/0x4a60c0. atre.py homolog confirmed the multilingual
// routine bidirectionally, and callsites found exactly those two deserializer
// callers in each build. The preview record builder and row-label formatter
// were then traced from the English save-list builder at 0x2aa10. Its
// multilingual homologue is 0x246da0; the corresponding direct calls identify
// 0x2498f0 and 0x249b00 independently of the weaker whole-function match.
constexpr ItemGuardAddrs kTotoriEn {
  0x25b3d0, 0x25b450, 0xd328d0,
  0xd22730, 0xd23b80, 0xd30670, 0xd30688,
  0x23eb30, 0x23eff0, 0x2891d0, 0x23a990, 0x2307d0, 0x25a010,
  0x230e50, 0x22ed20,
  0x286190, 0x2d490, 0x2db8e, 0x2d6a0,
  { 0x45, 0x33, 0xc0, 0x4c, 0x8d, 0x0d, 0xc6, 0xa2,
    0x9e, 0x00, 0x48, 0x83, 0xc1, 0x20, 0x66, 0x90 },
  { 0x45, 0x33, 0xc0, 0x4c, 0x8d, 0x49, 0x0c, 0x4c,
    0x8d, 0x15, 0x12, 0xda, 0x9e, 0x00, 0x66, 0x90 },
  { 0x40, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0xc7,
    0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48 },
  { 0x48, 0x8b, 0xc4, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x40, 0x0b },
  { 0x40, 0x53, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b,
    0x05, 0xf3, 0x37, 0xa2, 0x00, 0x48, 0x33, 0xc4 },
  { 0x4c, 0x8b, 0x02, 0x48, 0x8d, 0x0d, 0x96, 0x7d,
    0xae, 0x00, 0x49, 0x8b, 0xc0, 0xba, 0x28, 0x00 },
  { 0x40, 0x57, 0x48, 0x83, 0xec, 0x70, 0x48, 0xc7,
    0x44, 0x24, 0x28, 0xfe, 0xff, 0xff, 0xff, 0x48 },
  { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 },
  { 0x83, 0xf9, 0x09, 0x77, 0x32, 0x48, 0x63, 0xc1,
    0x4c, 0x8d, 0x0d, 0x41, 0x44, 0xa1, 0x00, 0x4d },
  { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48 },
  { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
    0xd9, 0x48, 0x8d, 0x51, 0x20, 0x48, 0x83, 0xc1 },
  { 0x40, 0x57, 0xb8, 0xc0, 0x10, 0x00, 0x00, 0xe8,
    0xd4, 0x8d, 0x56, 0x00, 0x48, 0x2b, 0xe0, 0x48 },
  { 0x40, 0x57, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00,
    0x00, 0x48, 0xc7, 0x44, 0x24, 0x30, 0xfe, 0xff },
};
constexpr ItemGuardAddrs kTotoriMulti {
  0x477d00, 0x477d80, 0x109aa50,
  0x108a8b0, 0x108bd00, 0x10987f0, 0x1098808,
  0x45b340, 0x45b800, 0x4a60c0, 0x456600, 0x44cf80, 0x476940,
  0x44d600, 0x44b4d0,
  0x4a3080, 0x2498f0, 0x24a14e, 0x249b00,
  { 0x45, 0x33, 0xc0, 0x4c, 0x8d, 0x0d, 0x46, 0xa4,
    0xc2, 0x00, 0x48, 0x83, 0xc1, 0x20, 0x66, 0x90 },
  { 0x45, 0x33, 0xc0, 0x4c, 0x8d, 0x49, 0x0c, 0x4c,
    0x8d, 0x15, 0x92, 0xdb, 0xc2, 0x00, 0x66, 0x90 },
  { 0x40, 0x57, 0x48, 0x83, 0xec, 0x50, 0x48, 0xc7,
    0x44, 0x24, 0x20, 0xfe, 0xff, 0xff, 0xff, 0x48 },
  { 0x48, 0x8b, 0xc4, 0x56, 0x57, 0x41, 0x54, 0x41,
    0x56, 0x41, 0x57, 0x48, 0x81, 0xec, 0x40, 0x0b },
  { 0x40, 0x53, 0x48, 0x83, 0xec, 0x50, 0x48, 0x8b,
    0x05, 0xb3, 0x5e, 0xb6, 0x00, 0x48, 0x33, 0xc4 },
  { 0x48, 0x8b, 0x02, 0x48, 0x8d, 0x0d, 0x66, 0x41,
    0xc3, 0x00, 0x0f, 0x10, 0x00, 0x0f, 0x11, 0x01 },
  { 0x40, 0x57, 0x48, 0x83, 0xec, 0x70, 0x48, 0xc7,
    0x44, 0x24, 0x28, 0xfe, 0xff, 0xff, 0xff, 0x48 },
  { 0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 },
  { 0x83, 0xf9, 0x09, 0x77, 0x32, 0x48, 0x63, 0xc1,
    0x4c, 0x8d, 0x15, 0x61, 0x12, 0xb7, 0x00, 0x4d },
  { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c,
    0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48 },
  { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
    0xd9, 0x48, 0x8d, 0x51, 0x20, 0x48, 0x83, 0xc1 },
  { 0x40, 0x57, 0xb8, 0xc0, 0x10, 0x00, 0x00, 0xe8,
    0x74, 0x9f, 0x5c, 0x00, 0x48, 0x2b, 0xe0, 0x48 },
  { 0x40, 0x57, 0x48, 0x81, 0xec, 0x80, 0x00, 0x00,
    0x00, 0x48, 0xc7, 0x44, 0x24, 0x30, 0xfe, 0xff },
};

const ItemGuardAddrs* addressesFor(const Game& game) {
  if (game.atlasVariant != AtlasTotori)
    return nullptr;
  return game.exeBuild == BuildEnglish ? &kTotoriEn : &kTotoriMulti;
}

struct TableSpan {
  uintptr_t base = 0;
  int32_t   recordLimit = 0;   // in-range indices are [0, recordLimit)
};

TableSpan g_effectTable;
TableSpan g_traitTable;
uintptr_t g_arrayBase = 0;
uintptr_t g_arrayEnd = 0;
uintptr_t g_carriedArray = 0;
uintptr_t g_containerArray = 0;
uintptr_t g_carriedOwner = 0;
uintptr_t g_containerOwner = 0;

ScanProc originalEffectScan = nullptr;
ScanProc originalTraitScan = nullptr;
SaveProc originalSaveWriter = nullptr;
SaveProc originalSaveLoader = nullptr;
ActualLoadProc originalActualLoad = nullptr;
SaveProc originalInventoryLoader = nullptr;
PreviewLoadProc originalPreviewLoad = nullptr;
PreviewRecordProc originalPreviewRecord = nullptr;
PreviewLabelProc originalPreviewLabel = nullptr;
EquipProc originalEquipMutation = nullptr;
ItemEffectBuildProc originalItemEffectBuild = nullptr;
RecalcProc recalcEquipmentSet = nullptr;
uintptr_t g_skillTable = 0;
uintptr_t g_previewRecordRet = 0;
thread_local bool g_actualSaveLoad = false;
thread_local bool g_previewLoadActive = false;
thread_local bool g_previewNeedsRepair = false;
thread_local bool g_previewResultReady = false;
thread_local bool g_previewResultNeedsRepair = false;

constexpr size_t kSaveSlotCount = 99;
std::array<std::atomic<bool>, kSaveSlotCount> g_saveNeedsRepair{};

using ItemBytes = std::array<BYTE, kItemStride>;
constexpr size_t kItemCount = kSetCount * 3;
std::array<ItemBytes, kItemCount> g_itemShadow{};
std::mutex g_itemTraceMutex;
bool g_itemShadowReady = false;
uint32_t g_unexpectedChanges = 0;

std::atomic<uint32_t> g_rejections{0};
constexpr uint32_t kMaxProbeLines = 64;

// Damaged records already reported, so each is named once in a normal log.
// Eight is well past what a party of ten with three slots each can plausibly
// carry and still be playable.
constexpr size_t kMaxReportedItems = 8;
std::atomic<uintptr_t> g_reportedItems[kMaxReportedItems];

// Decode `lea reg, [rip + disp32]` at `insn`, whose displacement sits at
// `dispOffset` bytes in and whose total length is `length`.
uintptr_t ripTarget(const BYTE* insn, size_t dispOffset, size_t length) {
  int32_t disp = 0;
  std::memcpy(&disp, insn + dispOffset, sizeof(disp));
  return reinterpret_cast<uintptr_t>(insn) + length + intptr_t(disp);
}

// How many whole records fit between `base` and the end of the committed region
// it sits in. This is only a backstop: the games' .data carries an enormous
// virtual size, so the answer runs to millions and admits plenty of indices
// that are wrong without being fatal. `limit` is the real bound when known.
TableSpan measureTable(uintptr_t base, int32_t limit) {
  TableSpan span;
  MEMORY_BASIC_INFORMATION mbi = {};
  if (!base || !VirtualQuery(reinterpret_cast<void*>(base), &mbi, sizeof(mbi)))
    return span;
  if (mbi.State != MEM_COMMIT)
    return span;
  const uintptr_t regionEnd =
    reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  if (regionEnd <= base)
    return span;
  span.base = base;
  span.recordLimit = int32_t((regionEnd - base) / kRecordStride);
  if (limit > 0 && limit < span.recordLimit)
    span.recordLimit = limit;
  return span;
}

// The engine's own test, plus the bound it omits. Negative stays "empty slot".
bool indexUsable(const TableSpan& table, int32_t index) {
  return index < 0 || uint32_t(index) < uint32_t(table.recordLimit);
}

float asFloat(uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Both tables share one record layout; the trait scan's base simply points
// 0x10 into it, which is why the two tables sit 0x37d0 rather than 0x37c0
// apart. The display name is at +0x30 of that shared layout, so it is +0x30
// from an effect record and +0x20 from a trait one.
constexpr size_t kEffectNameOffset = 0x30;
constexpr size_t kTraitNameOffset  = 0x20;

// Copy a table record's display name out of the executable's read-only data.
// Bounded and guarded: a name is only read when the record is in range, and the
// string itself is length-limited and range-checked before it is touched.
bool recordName(const TableSpan& table, size_t nameOffset, int32_t index,
                char* out, size_t size) {
  out[0] = '\0';
  if (!indexUsable(table, index) || index < 0)
    return false;
  uintptr_t text = 0;
  if (!tryRead(table.base + uintptr_t(index) * kRecordStride + nameOffset,
               text) || !text)
    return false;
  size_t span = size - 1;
  while (span && !readableRange(text, span))
    span /= 2;
  if (!span)
    return false;
  std::memcpy(out, reinterpret_cast<const void*>(text), span);
  out[span] = '\0';
  for (size_t i = 0; i < span; ++i) {
    // Anything unprintable means this is not the string table we expect.
    if (out[i] && (out[i] < 0x20 || out[i] == 0x7f)) {
      out[i] = '\0';
      break;
    }
  }
  return out[0] != '\0';
}

// "161 Sturdy" for a resolvable index, "2850 (no record)" for one the table
// cannot contain, "-" for an empty slot. This is what makes a report legible:
// it shows at a glance that the traits are real and the effects are not.
std::string describeIndices(const TableSpan& table, size_t nameOffset,
                            uintptr_t item, size_t offset, size_t count) {
  std::string text;
  for (size_t i = 0; i < count; ++i) {
    int32_t index = 0;
    if (!tryRead(item + offset + i * sizeof(int32_t), index))
      break;
    if (!text.empty())
      text += ", ";
    if (index < 0) {
      text += '-';
      continue;
    }
    text += std::to_string(index);
    char name[64] = {};
    if (recordName(table, nameOffset, index, name, sizeof(name))) {
      text += ' ';
      text += name;
    } else {
      text += " (no record)";
    }
  }
  return text;
}

void describeItem(uintptr_t item, int& set, int& slot) {
  set = -1;
  slot = -1;
  if (!g_arrayBase || item < g_arrayBase || item >= g_arrayEnd)
    return;
  const size_t offset = item - g_arrayBase;
  set = int(offset / kSetStride);
  const size_t inSet = offset % kSetStride;
  if (inSet >= kItemBase && (inSet - kItemBase) % kItemStride == 0)
    slot = int((inSet - kItemBase) / kItemStride);
}

// Claim a slot for `item`, returning true the first time it is seen. Racy in
// the way a report can afford: a tie logs the same record twice.
bool firstSightOfItem(uintptr_t item) {
  for (size_t i = 0; i < kMaxReportedItems; ++i) {
    const uintptr_t claimed = g_reportedItems[i].load(std::memory_order_relaxed);
    if (claimed == item)
      return false;
    if (!claimed) {
      uintptr_t expected = 0;
      if (g_reportedItems[i].compare_exchange_strong(expected, item,
            std::memory_order_relaxed))
        return true;
      --i;   // another thread took this slot; look at it again
    }
  }
  return false;
}

// Report a rejected index. Each damaged record is reported once without any
// switch, because a bug report is only useful if it names every affected piece
// of equipment rather than whichever one happened to be scanned first. Repeats
// of a record already reported need the probe switch, since a damaged record is
// re-read on every query and would otherwise fill the file.
void reportRejection(const char* scan, const char* field, size_t fieldOffset,
                     uintptr_t item, size_t fieldIndex, int32_t value,
                     int32_t type, int32_t limit, const void* caller) {
  const uint32_t seen = g_rejections.fetch_add(1, std::memory_order_relaxed);
  const bool novel = firstSightOfItem(item);
  if ((!novel && !probeEnabled()) || seen >= kMaxProbeLines)
    return;
  int set = 0, slot = 0;
  describeItem(item, set, slot);
  log("ITEMGUARD ", scan, " skipped out-of-range index: set=", set,
      " slot=", slot, " ", field, "[", fieldIndex, "] addr=0x", std::hex,
      item + fieldOffset + fieldIndex * sizeof(int32_t),
      " value=0x", uint32_t(value), std::dec, " (", value,
      ", ", asFloat(uint32_t(value)), " as float) limit=", limit,
      " queried_type=0x", std::hex, uint32_t(type),
      " caller=0x", reinterpret_cast<uintptr_t>(caller), std::dec);
  std::array<uint32_t, 13> raw{};
  if (tryRead(item, raw)) {
    log("ITEMGUARD   item @0x", std::hex, item, std::dec,
        " id=", int32_t(raw[0]), ",", int32_t(raw[1]),
        " quality=", asFloat(raw[2]), " tail=", int32_t(raw[12]));
    log("ITEMGUARD   traits: ", describeIndices(g_traitTable,
        kTraitNameOffset, item, kTraitOffset, kTraitCount));
    log("ITEMGUARD   effects: ", describeIndices(g_effectTable,
        kEffectNameOffset, item, kEffectOffset, kEffectCount));
  }
  if (seen == 0 && !probeEnabled())
    log("ITEMGUARD   this equipment record is damaged in the save file; the "
        "scan is being bounded so it cannot fault. ARLAND_ITEM_PROBE=1 logs "
        "every occurrence.");
}

// True when every index in the array is one the original loop can follow. One
// membership test covers the common case, because the item almost always lives
// in the global array measured at install.
bool indicesUsable(uintptr_t item, size_t offset, size_t count,
                   const TableSpan& table) {
  const bool inArray = g_arrayBase && item >= g_arrayBase &&
    item + kItemStride <= g_arrayEnd;
  if (!inArray && !readableRange(item, kItemStride))
    return false;
  for (size_t i = 0; i < count; ++i) {
    int32_t index = 0;
    std::memcpy(&index,
      reinterpret_cast<const void*>(item + offset + i * sizeof(int32_t)),
      sizeof(index));
    if (!indexUsable(table, index))
      return false;
  }
  return true;
}

// Reimplementations, reached only once a scan has been found unfollowable.
// Each mirrors the engine loop exactly except that an index it cannot follow is
// skipped the way a negative one already is.
int STDMETHODCALLTYPE effectScanDetour(uintptr_t item, int32_t type) {
  if (indicesUsable(item, kEffectOffset, kEffectCount, g_effectTable))
    return originalEffectScan(item, type);

  const void* caller = arlandReturnAddress();
  for (size_t i = 0; i < kEffectCount; ++i) {
    int32_t index = 0;
    if (!tryRead(item + kEffectOffset + i * sizeof(int32_t), index))
      break;
    if (index < 0)
      continue;
    if (!indexUsable(g_effectTable, index)) {
      reportRejection("effect scan", "effects", kEffectOffset, item, i, index,
                      type, g_effectTable.recordLimit, caller);
      continue;
    }
    int32_t recordType = 0;
    if (tryRead(g_effectTable.base + uintptr_t(index) * kRecordStride,
                recordType) && recordType == type)
      return int(i);
  }
  return -1;
}

int STDMETHODCALLTYPE traitScanDetour(uintptr_t item, int32_t type) {
  if (indicesUsable(item, kTraitOffset, kTraitCount, g_traitTable))
    return originalTraitScan(item, type);

  const void* caller = arlandReturnAddress();
  for (size_t i = 0; i < kTraitCount; ++i) {
    int32_t index = 0;
    if (!tryRead(item + kTraitOffset + i * sizeof(int32_t), index))
      break;
    if (index < 0)
      continue;
    if (!indexUsable(g_traitTable, index)) {
      reportRejection("trait scan", "traits", kTraitOffset, item, i, index,
                      type, g_traitTable.recordLimit, caller);
      continue;
    }
    // The engine compares three category fields per trait record, at +0x00,
    // +0x0c and +0x18.
    const uintptr_t record =
      g_traitTable.base + uintptr_t(index) * kRecordStride;
    for (size_t field = 0; field < 3; ++field) {
      int32_t recordType = 0;
      if (tryRead(record + field * 0x0c, recordType) && recordType == type)
        return int(i);
    }
  }
  return -1;
}

int32_t itemWord(uintptr_t item, size_t index) {
  int32_t value = 0;
  std::memcpy(&value,
              reinterpret_cast<const void*>(item + index * sizeof(value)),
              sizeof(value));
  return value;
}

void setItemWord(uintptr_t item, size_t index, int32_t value) {
  std::memcpy(reinterpret_cast<void*>(item + index * sizeof(value)),
              &value, sizeof(value));
}

bool itemIndexFieldsUsable(uintptr_t item, size_t first, size_t count,
                           const TableSpan& table) {
  for (size_t i = 0; i < count; ++i) {
    if (!indexUsable(table, itemWord(item, first + i)))
      return false;
  }
  return true;
}

bool qualityUsable(uintptr_t item) {
  const float quality = asFloat(uint32_t(itemWord(item, 2)));
  return std::isfinite(quality) && quality >= 0.0f && quality <= 120.0f &&
    (quality == 0.0f || quality >= 1.0f);
}

bool canonicalEmptyItem(uintptr_t item) {
  if (itemWord(item, 0) != -1 || itemWord(item, 1) != -1 ||
      asFloat(uint32_t(itemWord(item, 2))) != 50.0f)
    return false;
  for (size_t i = 3; i < 13; ++i) {
    if (itemWord(item, i) != -1)
      return false;
  }
  return true;
}

bool itemStructurallyUsable(uintptr_t item) {
  const int32_t first = itemWord(item, 0);
  const int32_t second = itemWord(item, 1);
  if (first < -1 || second < -1 || !qualityUsable(item))
    return false;
  if (first == -1 && second == -1)
    return canonicalEmptyItem(item);
  if (!itemIndexFieldsUsable(item, kTraitOffset / 4, kTraitCount,
                             g_traitTable) ||
      !itemIndexFieldsUsable(item, kEffectOffset / 4, kEffectCount,
                             g_effectTable))
    return false;
  const int32_t tail = itemWord(item, 12);
  return tail >= -1 && tail <= 0xffff;
}

bool itemPrefixUsable(uintptr_t item) {
  const int32_t first = itemWord(item, 0);
  const int32_t second = itemWord(item, 1);
  return first >= -1 && second >= -1 && (first >= 0 || second >= 0) &&
    qualityUsable(item) &&
    itemIndexFieldsUsable(item, kTraitOffset / 4, kTraitCount, g_traitTable);
}

void clearItem(uintptr_t item) {
  for (size_t i = 0; i < 13; ++i)
    setItemWord(item, i, -1);
  float quality = 50.0f;
  std::memcpy(reinterpret_cast<void*>(item + 8), &quality, sizeof(quality));
}

struct ItemRangeSummary {
  size_t salvaged = 0;
  size_t cleared = 0;
  size_t clampedLimits = 0;
};

ItemRangeSummary sanitizeItemRange(
    uintptr_t records, size_t count, const char* ownerName) {
  ItemRangeSummary summary;
  for (size_t slot = 0; slot < count; ++slot) {
    const uintptr_t item = records + slot * kItemStride;
    if (itemStructurallyUsable(item))
      continue;
    if (itemPrefixUsable(item)) {
      for (size_t i = 0; i < kEffectCount; ++i)
        setItemWord(item, kEffectOffset / 4 + i, -1);
      setItemWord(item, 12, -1);
      ++summary.salvaged;
      log("ITEMSANITIZE salvaged item prefix: owner=", ownerName,
          " slot=", slot, " id=", itemWord(item, 0), ",",
          itemWord(item, 1));
    } else {
      clearItem(item);
      ++summary.cleared;
      log("ITEMSANITIZE cleared unrecoverable item: owner=", ownerName,
          " slot=", slot);
    }
  }
  return summary;
}

bool itemRangeNeedsRepair(uintptr_t records, size_t count) {
  for (size_t slot = 0; slot < count; ++slot) {
    if (!itemStructurallyUsable(records + slot * kItemStride))
      return true;
  }
  return false;
}

bool itemListGeometryValid(
    uintptr_t owner, int32_t kind, size_t capacity, uintptr_t records) {
  int32_t liveKind = -1;
  int32_t liveCapacity = -1;
  uintptr_t liveRecords = 0;
  return tryRead(owner, liveKind) &&
    tryRead(owner + 8, liveCapacity) &&
    tryRead(owner + 0x10, liveRecords) &&
    liveKind == kind &&
    liveCapacity == int32_t(capacity) &&
    liveRecords == records &&
    readableRange(records, capacity * kItemStride);
}

bool itemListLimitNeedsRepair(uintptr_t owner, size_t capacity) {
  int32_t limit = 0;
  return tryRead(owner + 4, limit) &&
    (limit < 0 || limit > int32_t(capacity));
}

bool clampItemListLimit(
    uintptr_t owner, size_t capacity, const char* ownerName) {
  int32_t limit = 0;
  if (!tryRead(owner + 4, limit))
    return false;
  const int32_t clamped =
    limit < 0 ? 0 :
    limit > int32_t(capacity) ? int32_t(capacity) : limit;
  if (clamped == limit)
    return false;
  std::memcpy(reinterpret_cast<void*>(owner + 4), &clamped, sizeof(clamped));
  log("ITEMSANITIZE clamped saved item-list limit: owner=", ownerName,
      " old=", limit, " new=", clamped,
      " physical_capacity=", capacity);
  return true;
}

bool loadedInventoryNeedsRepair() {
  if (!itemListGeometryValid(g_carriedOwner, 0, kCarriedCount,
                             g_carriedArray) ||
      !itemListGeometryValid(g_containerOwner, 1, kContainerCount,
                             g_containerArray))
    return false;
  return itemListLimitNeedsRepair(g_carriedOwner, kCarriedCount) ||
    itemListLimitNeedsRepair(g_containerOwner, kContainerCount) ||
    itemRangeNeedsRepair(g_carriedArray, kCarriedCount) ||
    itemRangeNeedsRepair(g_containerArray, kContainerCount);
}

ItemRangeSummary sanitizeLoadedInventory() {
  ItemRangeSummary summary;
  if (!sanitizeEnabled())
    return summary;
  if (!itemListGeometryValid(g_carriedOwner, 0, kCarriedCount,
                             g_carriedArray) ||
      !itemListGeometryValid(g_containerOwner, 1, kContainerCount,
                             g_containerArray)) {
    log("ITEMSANITIZE inventory skipped: live list geometry mismatch");
    return summary;
  }
  summary.clampedLimits +=
    clampItemListLimit(g_carriedOwner, kCarriedCount, "carried") ? 1 : 0;
  summary.clampedLimits +=
    clampItemListLimit(g_containerOwner, kContainerCount, "container") ? 1 : 0;
  const ItemRangeSummary carried =
    sanitizeItemRange(g_carriedArray, kCarriedCount, "carried");
  const ItemRangeSummary container =
    sanitizeItemRange(g_containerArray, kContainerCount, "container");
  summary.salvaged = carried.salvaged + container.salvaged;
  summary.cleared = carried.cleared + container.cleared;
  return summary;
}

std::atomic<uint32_t> g_builderRepairs{0};

void STDMETHODCALLTYPE itemEffectBuildDetour(
    uintptr_t item, uintptr_t outputVector) {
  if (!readableRange(item, kItemStride)) {
    const uint32_t seen =
      g_builderRepairs.fetch_add(1, std::memory_order_relaxed);
    if (seen == 0 || probeEnabled())
      log("ITEMGUARD modifier builder skipped unreadable item @0x",
          std::hex, item, std::dec);
    return;
  }

  const int32_t actionId = itemWord(item, 1);
  if (!actionItemIdUsable(actionId)) {
    const uint32_t seen =
      g_builderRepairs.fetch_add(1, std::memory_order_relaxed);
    if (seen == 0 || probeEnabled())
      log("ITEMGUARD modifier builder rejected action id=", actionId,
          " item @0x", std::hex, item, std::dec);
    return;
  }

  const bool usable =
    qualityUsable(item) &&
    itemIndexFieldsUsable(item, kEffectOffset / 4, kEffectCount,
                          g_effectTable);
  if (usable) {
    originalItemEffectBuild(item, outputVector);
    return;
  }

  alignas(16) ItemBytes repaired{};
  std::memcpy(repaired.data(), reinterpret_cast<const void*>(item),
              repaired.size());
  const uintptr_t local = reinterpret_cast<uintptr_t>(repaired.data());
  if (!qualityUsable(local)) {
    const float quality = 50.0f;
    std::memcpy(repaired.data() + 8, &quality, sizeof(quality));
  }
  for (size_t i = 0; i < kEffectCount; ++i) {
    const size_t word = kEffectOffset / 4 + i;
    if (!indexUsable(g_effectTable, itemWord(local, word)))
      setItemWord(local, word, -1);
  }

  const uint32_t seen =
    g_builderRepairs.fetch_add(1, std::memory_order_relaxed);
  if (seen == 0 || probeEnabled())
    log("ITEMGUARD modifier builder used sanitized local item @0x",
        std::hex, item, std::dec);
  originalItemEffectBuild(local, outputVector);
}

struct SkillSource {
  int32_t id = -1;
  uint8_t kind = 0;
  int8_t unlockLevel = -1;
  bool exists = false;
};

bool skillSource(size_t set, size_t slot, SkillSource& out) {
  uintptr_t records = 0;
  if (!g_skillTable || set >= kSetCount || slot >= kSkillCount ||
      !tryRead(g_skillTable + set * sizeof(uintptr_t), records) || !records)
    return false;
  // Lists are sentinel-terminated and not uniformly ten records long (the
  // first two source pointers are only 0x80 apart). Never index through an
  // earlier sentinel into padding or the following character's list.
  for (size_t i = 0; i < slot; ++i) {
    int32_t priorId = 0;
    if (!tryRead(records + i * 0x18, priorId))
      return false;
    if (priorId == -1)
      return true;
  }
  const uintptr_t record = records + slot * 0x18;
  if (!tryRead(record, out.id))
    return false;
  if (out.id == -1)
    return true;
  if (!tryRead(record + 4, out.kind) ||
      !tryRead(record + 5, out.unlockLevel))
    return false;
  out.exists = true;
  return true;
}

bool skillEntryValid(uintptr_t entry, const SkillSource& source) {
  int32_t id = 0, tail = 0;
  uint8_t flag = 0;
  std::array<uint8_t, 3> padding{};
  std::memcpy(&id, reinterpret_cast<const void*>(entry), sizeof(id));
  std::memcpy(&flag, reinterpret_cast<const void*>(entry + 4), sizeof(flag));
  std::memcpy(padding.data(), reinterpret_cast<const void*>(entry + 5),
              padding.size());
  std::memcpy(&tail, reinterpret_cast<const void*>(entry + 8), sizeof(tail));
  return id == source.id && flag <= 1 &&
    padding == std::array<uint8_t, 3>{} &&
    tail == (source.exists ? 1 : 0);
}

void rebuildSkills(uintptr_t set, size_t setIndex, int32_t level) {
  for (size_t slot = 0; slot < kSkillCount; ++slot) {
    SkillSource source;
    if (!skillSource(setIndex, slot, source))
      break;
    const uintptr_t entry = set + kSkillBase + slot * kSkillStride;
    uint8_t oldFlag = 0;
    std::memcpy(&oldFlag, reinterpret_cast<const void*>(entry + 4),
                sizeof(oldFlag));
    const bool preserveFlag = skillEntryValid(entry, source);
    uint8_t flag = 0;
    if (source.exists) {
      flag = preserveFlag ? oldFlag :
        uint8_t(source.kind != 2 && source.unlockLevel >= 0 &&
                level >= source.unlockLevel);
    }
    const int32_t tail = source.exists ? 1 : 0;
    std::memcpy(reinterpret_cast<void*>(entry), &source.id, sizeof(source.id));
    std::memcpy(reinterpret_cast<void*>(entry + 4), &flag, sizeof(flag));
    std::memset(reinterpret_cast<void*>(entry + 5), 0, 3);
    std::memcpy(reinterpret_cast<void*>(entry + 8), &tail, sizeof(tail));
  }
}

bool skillsNeedRebuild(uintptr_t set, size_t setIndex) {
  for (size_t slot = 0; slot < kSkillCount; ++slot) {
    SkillSource source;
    if (!skillSource(setIndex, slot, source))
      return false;
    if (!skillEntryValid(set + kSkillBase + slot * kSkillStride, source))
      return true;
  }
  return false;
}

bool loadedEquipmentNeedsRepair() {
  if (!g_skillTable || !recalcEquipmentSet)
    return false;
  for (size_t setIndex = 0; setIndex < kSetCount; ++setIndex) {
    const uintptr_t set = g_arrayBase + setIndex * kSetStride;
    for (size_t slot = 0; slot < 3; ++slot) {
      if (!itemStructurallyUsable(
            set + kItemBase + slot * kItemStride))
        return true;
    }

    int32_t character = 0;
    int32_t level = 0;
    int32_t levelCopy = 0;
    float relationship = 0.0f;
    uint32_t partyFlag = 0;
    std::memcpy(&character, reinterpret_cast<const void*>(set),
                sizeof(character));
    std::memcpy(&level, reinterpret_cast<const void*>(set + 4),
                sizeof(level));
    std::memcpy(&levelCopy, reinterpret_cast<const void*>(set + 0x17c),
                sizeof(levelCopy));
    std::memcpy(&relationship,
                reinterpret_cast<const void*>(set + 0x180),
                sizeof(relationship));
    std::memcpy(&partyFlag, reinterpret_cast<const void*>(set + 0x188),
                sizeof(partyFlag));
    if ((level > 0 && character != int32_t(setIndex)) ||
        skillsNeedRebuild(set, setIndex) ||
        levelCopy != level ||
        !std::isfinite(relationship) || relationship < 0.0f ||
        relationship > 100.0f ||
        (partyFlag != 0 && partyFlag != 0x100))
      return true;
  }
  return false;
}

struct SanitizeSummary {
  size_t sets = 0;
  size_t salvagedItems = 0;
  size_t clearedItems = 0;
  size_t rebuiltSkills = 0;
  size_t repairedHeaders = 0;
};

SanitizeSummary sanitizeLoadedEquipment() {
  SanitizeSummary summary;
  if (!sanitizeEnabled() || !g_skillTable || !recalcEquipmentSet)
    return summary;

  // Core party sets use 0x100 once that party-state flag has become active.
  // It is the safest fallback for a damaged value when the other live core
  // sets unanimously carry it; otherwise retain the constructor value zero.
  uint32_t partyFlagFallback = 0;
  size_t activeFlags = 0;
  for (size_t i = 0; i < 7; ++i) {
    uint32_t flag = 0;
    std::memcpy(&flag,
      reinterpret_cast<const void*>(g_arrayBase + i * kSetStride + 0x188),
      sizeof(flag));
    if (flag == 0x100)
      ++activeFlags;
  }
  if (activeFlags >= 3)
    partyFlagFallback = 0x100;

  for (size_t setIndex = 0; setIndex < kSetCount; ++setIndex) {
    const uintptr_t set = g_arrayBase + setIndex * kSetStride;
    bool changed = false;
    bool itemsChanged = false;

    for (size_t slot = 0; slot < 3; ++slot) {
      const uintptr_t item = set + kItemBase + slot * kItemStride;
      if (itemStructurallyUsable(item))
        continue;
      if (itemPrefixUsable(item)) {
        for (size_t i = 0; i < kEffectCount; ++i)
          setItemWord(item, kEffectOffset / 4 + i, -1);
        setItemWord(item, 12, -1);
        ++summary.salvagedItems;
        log("ITEMSANITIZE salvaged item prefix: set=", setIndex,
            " slot=", slot, " id=", itemWord(item, 0), ",",
            itemWord(item, 1));
      } else {
        clearItem(item);
        ++summary.clearedItems;
        log("ITEMSANITIZE cleared unrecoverable item: set=", setIndex,
            " slot=", slot);
      }
      changed = true;
      itemsChanged = true;
    }

    int32_t level = 0;
    std::memcpy(&level, reinterpret_cast<const void*>(set + 4), sizeof(level));
    int32_t character = 0;
    std::memcpy(&character, reinterpret_cast<const void*>(set),
                sizeof(character));
    if (level > 0 && character != int32_t(setIndex)) {
      const int32_t restored = int32_t(setIndex);
      std::memcpy(reinterpret_cast<void*>(set), &restored, sizeof(restored));
      ++summary.repairedHeaders;
      changed = true;
      log("ITEMSANITIZE restored character id: set=", setIndex,
          " old=", character, " new=", restored);
    }

    if (skillsNeedRebuild(set, setIndex)) {
      rebuildSkills(set, setIndex, level);
      ++summary.rebuiltSkills;
      changed = true;
      log("ITEMSANITIZE rebuilt character skills: set=", setIndex,
          " level=", level);
    }

    int32_t levelCopy = 0;
    float relationship = 0.0f;
    uint32_t partyFlag = 0;
    std::memcpy(&levelCopy, reinterpret_cast<const void*>(set + 0x17c),
                sizeof(levelCopy));
    std::memcpy(&relationship, reinterpret_cast<const void*>(set + 0x180),
                sizeof(relationship));
    std::memcpy(&partyFlag, reinterpret_cast<const void*>(set + 0x188),
                sizeof(partyFlag));
    const bool tailDamaged = levelCopy != level ||
      !std::isfinite(relationship) || relationship < 0.0f ||
      relationship > 100.0f || (partyFlag != 0 && partyFlag != 0x100);
    if (tailDamaged) {
      const int32_t zero = 0;
      std::memcpy(reinterpret_cast<void*>(set + 0x178), &zero, sizeof(zero));
      std::memcpy(reinterpret_cast<void*>(set + 0x17c), &level, sizeof(level));
      if (!std::isfinite(relationship) || relationship < 0.0f ||
          relationship > 100.0f) {
        relationship = 0.0f;
        std::memcpy(reinterpret_cast<void*>(set + 0x180), &relationship,
                    sizeof(relationship));
      }
      // The same overwriting record that destroys all three validated fields
      // also crosses +0x184. Its original value cannot be reconstructed, so
      // reset that secondary progress counter rather than retain injected data.
      std::memcpy(reinterpret_cast<void*>(set + 0x184), &zero, sizeof(zero));
      partyFlag = partyFlagFallback;
      std::memcpy(reinterpret_cast<void*>(set + 0x188), &partyFlag,
                  sizeof(partyFlag));
      ++summary.repairedHeaders;
      changed = true;
      log("ITEMSANITIZE repaired set tail: set=", setIndex,
          " level=", level, " relationship=", relationship,
          " party_flag=0x", std::hex, partyFlag, std::dec);
    }

    if (itemsChanged) {
      const int32_t zero = 0;
      std::memcpy(reinterpret_cast<void*>(set + 0x178), &zero, sizeof(zero));
      recalcEquipmentSet(set);
    }
    if (changed)
      ++summary.sets;
  }
  return summary;
}

uintptr_t itemAddress(size_t index) {
  const size_t set = index / 3;
  const size_t slot = index % 3;
  return g_arrayBase + set * kSetStride + kItemBase + slot * kItemStride;
}

void copyLiveItems(std::array<ItemBytes, kItemCount>& out) {
  for (size_t i = 0; i < out.size(); ++i)
    std::memcpy(out[i].data(), reinterpret_cast<const void*>(itemAddress(i)),
                kItemStride);
}

void logItemWords(const char* label, size_t index, const ItemBytes& item) {
  std::array<uint32_t, 13> words{};
  std::memcpy(words.data(), item.data(), item.size());
  log("ITEMSAVE ", label, " set=", index / 3, " slot=", index % 3,
      " words=", std::hex,
      words[0], ",", words[1], ",", words[2], ",", words[3], ",",
      words[4], ",", words[5], ",", words[6], ",", words[7], ",",
      words[8], ",", words[9], ",", words[10], ",", words[11], ",",
      words[12], std::dec);
}

size_t reportShadowDivergence(const char* boundary) {
  if (!g_itemShadowReady)
    return 0;
  size_t changed = 0;
  for (size_t i = 0; i < kItemCount; ++i) {
    ItemBytes live{};
    std::memcpy(live.data(), reinterpret_cast<const void*>(itemAddress(i)),
                live.size());
    if (live == g_itemShadow[i])
      continue;
    ++changed;
    if (g_unexpectedChanges++ >= kMaxProbeLines)
      continue;
    size_t byte = 0;
    while (byte < live.size() && live[byte] == g_itemShadow[i][byte])
      ++byte;
    log("ITEMSAVE UNEXPECTED equipment mutation before ", boundary,
        ": set=", i / 3, " slot=", i % 3, " first_byte=0x", std::hex,
        byte, " expected=0x", uint32_t(g_itemShadow[i][byte]),
        " actual=0x", uint32_t(live[byte]), std::dec);
    logItemWords("expected", i, g_itemShadow[i]);
    logItemWords("actual", i, live);
  }
  return changed;
}

void describeLoadedBaseline() {
  size_t occupied = 0;
  size_t invalidFields = 0;
  for (size_t i = 0; i < kItemCount; ++i) {
    std::array<uint32_t, 13> words{};
    std::memcpy(words.data(), g_itemShadow[i].data(), g_itemShadow[i].size());
    if (int32_t(words[0]) >= 0 || int32_t(words[1]) >= 0)
      ++occupied;
    for (size_t j = 0; j < kTraitCount; ++j) {
      const int32_t value = int32_t(words[kTraitOffset / 4 + j]);
      if (!indexUsable(g_traitTable, value))
        ++invalidFields;
    }
    for (size_t j = 0; j < kEffectCount; ++j) {
      const int32_t value = int32_t(words[kEffectOffset / 4 + j]);
      if (!indexUsable(g_effectTable, value))
        ++invalidFields;
    }
  }
  log("ITEMSAVE baseline captured after load: occupied=", occupied,
      " invalid_index_fields=", invalidFields);
}

bool parseSaveSlot(uintptr_t text, size_t& slot) {
  std::array<char, 11> name{};
  if (!readableRange(text, name.size()))
    return false;
  std::memcpy(name.data(), reinterpret_cast<const void*>(text), name.size());
  if (std::memcmp(name.data(), "GAMEDATA", 8) != 0 ||
      name[8] < '0' || name[8] > '9' ||
      name[9] < '0' || name[9] > '9' || name[10] != '\0')
    return false;
  slot = size_t(name[8] - '0') * 10 + size_t(name[9] - '0');
  return slot < kSaveSlotCount;
}

int STDMETHODCALLTYPE previewLoadDetour(uintptr_t self) {
  g_previewLoadActive = true;
  g_previewNeedsRepair = false;
  g_previewResultReady = false;
  g_previewResultNeedsRepair = false;
  const int result = originalPreviewLoad(self);
  g_previewLoadActive = false;
  g_previewResultReady = true;
  g_previewResultNeedsRepair = g_previewNeedsRepair;
  return result;
}

void STDMETHODCALLTYPE previewRecordDetour(uintptr_t name) {
  const uintptr_t caller =
    reinterpret_cast<uintptr_t>(arlandReturnAddress());
  size_t slot = 0;
  const bool namedSlot = parseSaveSlot(name, slot);
  originalPreviewRecord(name);
  if (caller != g_previewRecordRet)
    return;

  const bool needsRepair =
    namedSlot && g_previewResultReady && g_previewResultNeedsRepair;
  if (namedSlot) {
    const bool previous = g_saveNeedsRepair[slot].exchange(
      needsRepair, std::memory_order_relaxed);
    if (needsRepair && !previous)
      log("ITEMCHECK save slot=", slot + 1, " needs repair on load");
  }
  g_previewResultReady = false;
  g_previewResultNeedsRepair = false;
}

char* STDMETHODCALLTYPE previewLabelDetour(uintptr_t record) {
  char* result = originalPreviewLabel(record);
  size_t slot = 0;
  if (!parseSaveSlot(record + 8, slot) ||
      !g_saveNeedsRepair[slot].load(std::memory_order_relaxed))
    return result;

  constexpr size_t kLabelOffset = 0x849;
  constexpr size_t kLabelCapacity = 0x400;
  constexpr char kMarker[] = " [TBR]";
  char* const expected =
    reinterpret_cast<char*>(record + kLabelOffset);
  if (result != expected ||
      !readableRange(reinterpret_cast<uintptr_t>(result), kLabelCapacity))
    return result;

  size_t length = 0;
  while (length < kLabelCapacity && result[length])
    ++length;
  if (length == kLabelCapacity ||
      length + sizeof(kMarker) > kLabelCapacity)
    return result;

  size_t titleEnd = 0;
  while (titleEnd < length && result[titleEnd] != '\n')
    ++titleEnd;
  constexpr size_t kMarkerLength = sizeof(kMarker) - 1;
  std::memmove(result + titleEnd + kMarkerLength, result + titleEnd,
               length - titleEnd + 1);
  std::memcpy(result + titleEnd, kMarker, kMarkerLength);
  return result;
}

int STDMETHODCALLTYPE actualLoadDetour(uintptr_t self) {
  const bool previous = g_actualSaveLoad;
  g_actualSaveLoad = true;
  const int result = originalActualLoad(self);
  g_actualSaveLoad = previous;
  return result;
}

int STDMETHODCALLTYPE saveLoaderDetour(uintptr_t self, uintptr_t stream) {
  const int result = originalSaveLoader(self, stream);
  if (g_previewLoadActive) {
    g_previewNeedsRepair =
      g_previewNeedsRepair || loadedEquipmentNeedsRepair();
    return result;
  }
  if (!g_actualSaveLoad)
    return result;
  const SanitizeSummary repaired = sanitizeLoadedEquipment();
  if (repaired.sets) {
    log("ITEMSANITIZE repaired loaded save: sets=", repaired.sets,
        " salvaged_items=", repaired.salvagedItems,
        " cleared_items=", repaired.clearedItems,
        " rebuilt_skills=", repaired.rebuiltSkills,
        " repaired_headers=", repaired.repairedHeaders,
        "; save normally to persist");
  }
  if (!saveTraceEnabled())
    return result;
  std::lock_guard<std::mutex> lock(g_itemTraceMutex);
  copyLiveItems(g_itemShadow);
  g_itemShadowReady = true;
  g_unexpectedChanges = 0;
  describeLoadedBaseline();
  return result;
}

int STDMETHODCALLTYPE inventoryLoaderDetour(
    uintptr_t self, uintptr_t stream) {
  const int result = originalInventoryLoader(self, stream);
  if (g_previewLoadActive) {
    g_previewNeedsRepair =
      g_previewNeedsRepair || loadedInventoryNeedsRepair();
    return result;
  }
  if (!g_actualSaveLoad)
    return result;
  const ItemRangeSummary repaired = sanitizeLoadedInventory();
  if (repaired.salvaged || repaired.cleared || repaired.clampedLimits) {
    log("ITEMSANITIZE repaired loaded inventory: salvaged_items=",
        repaired.salvaged, " cleared_items=", repaired.cleared,
        " clamped_limits=", repaired.clampedLimits,
        "; save normally to persist");
  }
  return result;
}

int STDMETHODCALLTYPE saveWriterDetour(uintptr_t self, uintptr_t stream) {
  {
    std::lock_guard<std::mutex> lock(g_itemTraceMutex);
    const size_t changed = reportShadowDivergence("save");
    log("ITEMSAVE pre-save comparison: unexpected_records=", changed,
        changed ? " (game memory differs from tracked load/equip state)"
                : " (exact match)");
  }
  return originalSaveWriter(self, stream);
}

bool globalItemIndex(uintptr_t set, uint32_t slot, size_t& index) {
  if (slot >= 3 || set < g_arrayBase || set >= g_arrayEnd)
    return false;
  const size_t offset = set - g_arrayBase;
  if (offset % kSetStride)
    return false;
  const size_t setIndex = offset / kSetStride;
  if (setIndex >= kSetCount)
    return false;
  index = setIndex * 3 + slot;
  return true;
}

uintptr_t STDMETHODCALLTYPE equipMutationDetour(
    uintptr_t set, uintptr_t oldItem, uint32_t slot, uintptr_t newItem) {
  std::array<ItemBytes, kItemCount> before{};
  ItemBytes source{};
  size_t target = 0;
  bool tracked = false;
  bool sourceCopied = false;
  {
    std::lock_guard<std::mutex> lock(g_itemTraceMutex);
    tracked = g_itemShadowReady && globalItemIndex(set, slot, target);
    if (tracked) {
      reportShadowDivergence("equip/swap");
      copyLiveItems(before);
      if (newItem && readableRange(newItem, kItemStride)) {
        std::memcpy(source.data(), reinterpret_cast<const void*>(newItem),
                    source.size());
        sourceCopied = true;
      }
    }
  }

  const uintptr_t result =
    originalEquipMutation(set, oldItem, slot, newItem);

  if (!tracked)
    return result;
  std::lock_guard<std::mutex> lock(g_itemTraceMutex);
  for (size_t i = 0; i < kItemCount; ++i) {
    if (i == target)
      continue;
    ItemBytes live{};
    std::memcpy(live.data(), reinterpret_cast<const void*>(itemAddress(i)),
                live.size());
    if (live != before[i] && g_unexpectedChanges++ < kMaxProbeLines) {
      log("ITEMSAVE UNEXPECTED equip/swap changed a non-target record: target=",
          target / 3, "/", target % 3, " changed=", i / 3, "/", i % 3);
      logItemWords("before", i, before[i]);
      logItemWords("after", i, live);
    }
  }

  ItemBytes liveTarget{};
  std::memcpy(liveTarget.data(),
              reinterpret_cast<const void*>(itemAddress(target)),
              liveTarget.size());
  if (newItem && (!sourceCopied || liveTarget != source) &&
      g_unexpectedChanges++ < kMaxProbeLines) {
    log("ITEMSAVE UNEXPECTED equip/swap target differs from its source: set=",
        target / 3, " slot=", target % 3,
        " source_readable=", sourceCopied);
    if (sourceCopied)
      logItemWords("source", target, source);
    logItemWords("target", target, liveTarget);
  }
  g_itemShadow[target] = liveTarget;
  log("ITEMSAVE tracked equip/swap: set=", target / 3,
      " slot=", target % 3, " operation=", newItem ? "copy" : "clear");
  return result;
}

bool installItemSaveHandling(BYTE* base, const ItemGuardAddrs& addrs) {
  const bool wantSanitizer = sanitizeEnabled();
  const bool wantTrace = saveTraceEnabled();
  if (!wantSanitizer && !wantTrace) {
    log("FIXES item_save_sanitize=off");
    return false;
  }

  BYTE* writer = base + addrs.saveWriter;
  BYTE* loader = base + addrs.saveLoader;
  BYTE* actualLoad = base + addrs.actualLoad;
  BYTE* inventoryLoader = base + addrs.inventoryLoader;
  BYTE* equip = base + addrs.equipMutation;
  BYTE* skill = base + addrs.skillInfo;
  BYTE* recalc = base + addrs.recalc;
  BYTE* previewLoad = base + addrs.previewLoad;
  BYTE* previewRecord = base + addrs.previewRecord;
  BYTE* previewLabel = base + addrs.previewLabel;

  bool sanitizerReady = false;
  if (wantSanitizer) {
    sanitizerReady = matches(loader, addrs.loaderExpected) &&
      matches(actualLoad, addrs.actualLoadExpected) &&
      matches(inventoryLoader, addrs.inventoryLoaderExpected) &&
      matches(skill, addrs.skillExpected) &&
      matches(recalc, addrs.recalcExpected) &&
      matches(previewLoad, addrs.previewLoadExpected) &&
      matches(previewRecord, addrs.previewRecordExpected) &&
      matches(previewLabel, addrs.previewLabelExpected);
    if (sanitizerReady) {
      g_skillTable = ripTarget(skill + 8, 3, 7);
      recalcEquipmentSet = reinterpret_cast<RecalcProc>(recalc);
      g_previewRecordRet =
        reinterpret_cast<uintptr_t>(base) + addrs.previewRecordRet;
      sanitizerReady =
        readableRange(g_skillTable, kSetCount * sizeof(uintptr_t));
    }
    if (!sanitizerReady) {
      g_skillTable = 0;
      recalcEquipmentSet = nullptr;
      g_previewRecordRet = 0;
      log("FIXES item_save_sanitize=failed (address validation)");
    }
  }

  bool writerOk = false;
  bool equipOk = false;
  bool traceReady = false;
  if (wantTrace) {
    traceReady = matches(writer, addrs.writerExpected) &&
      matches(equip, addrs.equipExpected) &&
      matches(loader, addrs.loaderExpected) &&
      matches(actualLoad, addrs.actualLoadExpected);
  }

  // Install the display path from the outside in. Each hook is transparent
  // until the next one succeeds: the label sees no marked slots without the
  // record hook, and the record hook sees no preview result without the
  // preview scope.
  const bool previewLabelOk = sanitizerReady &&
    installMinHookDetour(previewLabel,
      reinterpret_cast<void*>(&previewLabelDetour),
      reinterpret_cast<void**>(&originalPreviewLabel));
  const bool previewRecordOk = previewLabelOk &&
    installMinHookDetour(previewRecord,
      reinterpret_cast<void*>(&previewRecordDetour),
      reinterpret_cast<void**>(&originalPreviewRecord));
  const bool previewLoadOk = previewRecordOk &&
    installMinHookDetour(previewLoad,
      reinterpret_cast<void*>(&previewLoadDetour),
      reinterpret_cast<void**>(&originalPreviewLoad));
  const bool previewMarkerOk =
    previewLabelOk && previewRecordOk && previewLoadOk;
  sanitizerReady = sanitizerReady && previewMarkerOk;

  // Totori calls the same chunk deserializers while building save-list
  // previews. Only the dedicated selected-save routine should authorize
  // repair or save-data change tracking. Install this scope first; by itself
  // it is transparent, so a later partial hook failure cannot change game
  // data.
  const bool actualLoadOk = (sanitizerReady || traceReady) &&
    installMinHookDetour(actualLoad,
      reinterpret_cast<void*>(&actualLoadDetour),
      reinterpret_cast<void**>(&originalActualLoad));
  sanitizerReady = sanitizerReady && actualLoadOk;
  traceReady = traceReady && actualLoadOk;

  if (wantTrace) {
    writerOk = traceReady && installMinHookDetour(writer,
      reinterpret_cast<void*>(&saveWriterDetour),
      reinterpret_cast<void**>(&originalSaveWriter));
    equipOk = writerOk && installMinHookDetour(equip,
      reinterpret_cast<void*>(&equipMutationDetour),
      reinterpret_cast<void**>(&originalEquipMutation));
    traceReady = writerOk && equipOk;
  }

  bool inventoryLoaderOk = false;
  if (sanitizerReady) {
    inventoryLoaderOk = installMinHookDetour(inventoryLoader,
        reinterpret_cast<void*>(&inventoryLoaderDetour),
        reinterpret_cast<void**>(&originalInventoryLoader));
  }

  // Install the shared loader detour last. It sanitizes independently of the
  // optional tracer; a partially installed trace hook remains transparent.
  const bool sanitizerHooksReady =
    sanitizerReady && inventoryLoaderOk;
  const bool loaderOk = (sanitizerHooksReady || traceReady) &&
    installMinHookDetour(loader,
    reinterpret_cast<void*>(&saveLoaderDetour),
    reinterpret_cast<void**>(&originalSaveLoader));
  if (wantSanitizer) {
    log("FIXES item_save_sanitize=",
        sanitizerHooksReady && loaderOk ? "active" : "failed",
        " actual_load=", actualLoadOk,
        " equipment_loader=", loaderOk,
        " inventory_loader=", inventoryLoaderOk,
        " preview_marker=", previewMarkerOk);
  }
  if (wantTrace) {
    log("DIAGNOSTICS item_save_trace=",
        traceReady && loaderOk ? "active" : "failed",
        " actual_load=", actualLoadOk,
        " writer=", writerOk, " equip=", equipOk, " loader=", loaderOk);
  }
  return loaderOk || inventoryLoaderOk;
}

}  // namespace

bool installItemGuard(BYTE* base, const Game& game) {
  const ItemGuardAddrs* addrs = addressesFor(game);
  if (!addrs) {
    // Rorona and Meruru have no byte-match for either leaf: their item struct
    // is a different engine generation, and neither reports this crash.
    log("FIXES item_scan_guard=not_applicable");
    return false;
  }
  BYTE* effect = base + addrs->effectScan;
  BYTE* trait = base + addrs->traitScan;
  if (!matches(effect, addrs->effectExpected) ||
      !matches(trait, addrs->traitExpected)) {
    log("Item scan guard prologue mismatch; not installing");
    return false;
  }

  // Table bases come out of each leaf's own lea: `lea r9, [rip+d]` is seven
  // bytes from leaf+3, and `lea r10, [rip+d]` is seven bytes from leaf+7.
  const uintptr_t effectTable = ripTarget(effect + 3, 3, 7);
  const uintptr_t traitTable = ripTarget(trait + 7, 3, 7);
  const int32_t effectLimit = traitTable > effectTable
    ? int32_t((traitTable - effectTable) / kRecordStride) : 0;
  g_effectTable = measureTable(effectTable, effectLimit);
  g_traitTable = measureTable(traitTable, kTraitRecordLimit);
  g_arrayBase = reinterpret_cast<uintptr_t>(base) + addrs->equipArray;
  g_arrayEnd = g_arrayBase + kSetStride * kSetCount;
  g_carriedArray =
    reinterpret_cast<uintptr_t>(base) + addrs->carriedArray;
  g_containerArray =
    reinterpret_cast<uintptr_t>(base) + addrs->containerArray;
  g_carriedOwner =
    reinterpret_cast<uintptr_t>(base) + addrs->carriedOwner;
  g_containerOwner =
    reinterpret_cast<uintptr_t>(base) + addrs->containerOwner;
  if (!g_effectTable.recordLimit || !g_traitTable.recordLimit ||
      !readableRange(g_arrayBase, kSetStride * kSetCount) ||
      !readableRange(g_carriedArray, kCarriedCount * kItemStride) ||
      !readableRange(g_containerArray, kContainerCount * kItemStride) ||
      !readableRange(g_carriedOwner, 0x18) ||
      !readableRange(g_containerOwner, 0x18)) {
    log("Item scan guard: tables or item arrays not mapped; not installing");
    return false;
  }

  bool effectOk = false;
  bool traitOk = false;
  bool itemEffectBuildOk = false;
  if (guardEnabled()) {
    effectOk = installMinHookDetour(effect,
      reinterpret_cast<void*>(&effectScanDetour),
      reinterpret_cast<void**>(&originalEffectScan));
    traitOk = installMinHookDetour(trait,
      reinterpret_cast<void*>(&traitScanDetour),
      reinterpret_cast<void**>(&originalTraitScan));
    BYTE* itemEffectBuild = base + addrs->itemEffectBuild;
    itemEffectBuildOk = matches(
      itemEffectBuild, addrs->itemEffectBuildExpected) &&
      installMinHookDetour(itemEffectBuild,
        reinterpret_cast<void*>(&itemEffectBuildDetour),
        reinterpret_cast<void**>(&originalItemEffectBuild));
  }
  const bool installed = effectOk && traitOk && itemEffectBuildOk;
  log("FIXES item_scan_guard=",
      guardEnabled() ? (installed ? "active" : "failed") : "off",
      " effects=", g_effectTable.recordLimit,
      " traits=", g_traitTable.recordLimit);
  if (verboseLogging())
    log("Item scan guard effect_rva=0x", std::hex, addrs->effectScan,
        " trait_rva=0x", addrs->traitScan,
        " effect_table=0x", g_effectTable.base,
        " trait_table=0x", g_traitTable.base,
        " equipment=0x", g_arrayBase, std::dec,
        " effect_hook=", effectOk, " trait_hook=", traitOk,
        " modifier_hook=", itemEffectBuildOk);
  const bool saveHandling = installItemSaveHandling(base, *addrs);
  return installed || saveHandling;
}

}  // namespace atfix
