// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../../core/hook_util.h"

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
// design: the engine's other consumer of a trait index opens with
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
// item record and the caller, rather than the one line each damaged record
// gets by default.
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
//
// A save whose preview scan comes back damaged is marked " [TBR]" in the load
// list, so a damaged slot is visible before it is loaded. Repair happens in
// memory: the player has to save normally to keep it, and the log says so.
// ARLAND_ITEM_SANITIZE=0 disables persistent recovery for comparison.
// ARLAND_ITEM_SAVE_TRACE=1 is a separate save-data change-tracking diagnostic:
// it shadows only the 30 saved item records, mirrors the engine's central
// equip/swap operation, and compares live memory with that model immediately
// before each save. An unexpected difference proves that some other in-memory
// route changed equipment before the serializer's flat copy.
namespace atfix {

// Totori only, both builds, on unless ARLAND_ITEM_GUARD=0. Rorona and Meruru
// have no equivalent scan to guard.
bool installItemGuard(BYTE* base, const Game& game);

}  // namespace atfix
