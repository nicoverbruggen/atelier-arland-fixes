// SPDX-License-Identifier: MIT
//
// Implementation. What this fixes and why it takes this shape is in
// shop_fix.h; what is here is the per-build wiring and the notes that
// only mean anything beside the code they sit on.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstdint>

#include "../../core/log.h"
#include "../../core/mem.h"
#include "shop_fix.h"

namespace atfix {

extern Log log;  // main.cpp

namespace {

using ShopGoodsUpdateProc = bool (STDMETHODCALLTYPE*)(uintptr_t, float);

constexpr uintptr_t kShopGoodsUpdateEn = 0x32d530;
constexpr uintptr_t kShopGoodsUpdateMulti = 0x54b8d0;
constexpr uintptr_t kRowsBeginOffset = 0x60;
constexpr uintptr_t kRowsEndOffset = 0x68;
constexpr uintptr_t kInputOffset = 0x48;
constexpr uintptr_t kInputStateOffset = 0x50;
constexpr uintptr_t kPreviousRowOffset = 0x54;
constexpr uintptr_t kInputValueOffset = 0x28;
constexpr uintptr_t kRowValueOffset = 0x4c;
constexpr uintptr_t kRowStride = 0x54;
constexpr int32_t kCommitInputState = 2;

// Byte-identical in the English and multilingual executables.
constexpr std::array<BYTE, 16> kShopGoodsUpdateExpected = {
  0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b,
  0x41, 0x68, 0x48, 0x8b, 0xd9, 0x0f, 0x29, 0x74,
};

ShopGoodsUpdateProc originalShopGoodsUpdate = nullptr;

bool STDMETHODCALLTYPE correctedShopGoodsUpdate(uintptr_t self,
                                                 float elapsed) {
  uintptr_t begin = 0;
  uintptr_t end = 0;
  uintptr_t input = 0;
  int32_t inputState = 0;
  int32_t previousRow = -1;
  const bool readable =
    tryRead(self + kRowsBeginOffset, begin) &&
    tryRead(self + kRowsEndOffset, end) &&
    tryRead(self + kInputOffset, input) &&
    tryRead(self + kInputStateOffset, inputState) &&
    tryRead(self + kPreviousRowOffset, previousRow);

  // Vanilla returns before the vulnerable branch when the vector is empty.
  // For a non-empty vector, state 2 and a live input object are the two gates
  // immediately preceding the unchecked indexed store.
  const bool validShape = end >= begin &&
    (end - begin) % kRowStride == 0;
  const uint64_t rowCount = validShape
    ? (end - begin) / kRowStride : 0;
  const bool invalidPreviousRow = readable && begin != end &&
    inputState == kCommitInputState && input &&
    (!validShape || previousRow < 0 || uint64_t(previousRow) >= rowCount);

  if (invalidPreviousRow)
    *reinterpret_cast<int32_t*>(self + kInputStateOffset) = 0;

  const bool result = originalShopGoodsUpdate(self, elapsed);

  if (invalidPreviousRow) {
    // Put the commit state back only if the original left it as it was set
    // above, so the shop retries the commit next frame. If the original
    // advanced its own state machine during the call, that value is newer than
    // the one saved here and overwriting it would rewind the shop.
    int32_t after = 0;
    if (tryRead(self + kInputStateOffset, after) && after == 0)
      *reinterpret_cast<int32_t*>(self + kInputStateOffset) = inputState;
    const uintptr_t target = begin +
      uintptr_t(intptr_t(previousRow) * intptr_t(kRowStride)) +
      kRowValueOffset;
    int32_t value = 0;
    tryRead(input + kInputValueOffset, value);
    log("SHOP GOODS prevented out-of-bounds previous-row write begin=0x",
        std::hex, begin, " target=0x", target, std::dec,
        " rows=", rowCount, " index=", previousRow,
        " value=", value);
  }
  return result;
}

}  // namespace

bool installShopFix(BYTE* base, const Game& game) {
  if (game.atlasVariant != AtlasTotori) {
    log("FIXES shop_goods_bounds=not_applicable");
    return false;
  }

  const uintptr_t rva = game.exeBuild == BuildEnglish
    ? kShopGoodsUpdateEn : kShopGoodsUpdateMulti;
  BYTE* target = base + rva;
  if (!matches(target, kShopGoodsUpdateExpected)) {
    log("ShopGoodsList bounds-fix prologue mismatch rva=0x",
        std::hex, rva, std::dec, "; not installing");
    return false;
  }

  const bool installed = installMinHookDetour(
    target, reinterpret_cast<void*>(&correctedShopGoodsUpdate),
    reinterpret_cast<void**>(&originalShopGoodsUpdate));
  log("FIXES shop_goods_bounds=", installed ? "active" : "failed");
  return installed;
}

}  // namespace atfix
