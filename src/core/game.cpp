// SPDX-License-Identifier: MIT
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdlib>
#include <cstring>

#include "game.h"

namespace atfix {

// Defined in config.cpp: reads an ini bool and seeds the default key when it
// is absent (so the option is discoverable in arland-fix.ini).
bool arlandConfigBool(const char* section, const char* key, bool def);

namespace {

const char* baseName(const char* path) {
  const char* back = std::strrchr(path, '\\');
  const char* forward = std::strrchr(path, '/');
  const char* sep = back > forward ? back : forward;
  return sep ? sep + 1 : path;
}

Title detectTitle() {
  HMODULE module = GetModuleHandleW(nullptr);
  char path[MAX_PATH] = {};
  if (!module || !GetModuleFileNameA(module, path, sizeof(path)))
    return Title::Unknown;
  const char* name = baseName(path);
  if (!_strnicmp(name, "A11R", 4)) return Title::Rorona;
  if (!_strnicmp(name, "A12V", 4)) return Title::Totori;
  if (!_strnicmp(name, "A13V", 4)) return Title::Meruru;
  return Title::Unknown;
}

// Where a feature's override lives. `invert` is set when the ini/env value is
// worded as the inverse of the feature: BattleCutInDimming asks "may the cut-in
// dim the scene?", the opposite of the CutInDimHold action. A null section/key
// means the feature has no bool ini key (core features, or valued knobs read by
// their own readers).
struct Descriptor {
  const char* env;
  const char* section;
  const char* key;
  bool invert;
};

const Descriptor& descriptor(Feature f) {
  static const Descriptor table[static_cast<int>(Feature::Count)] = {
    /* SyncFix            */ { nullptr, nullptr, nullptr, false },
    /* MenuHitchFix       */ { "ARLAND_MENU_FIX", nullptr, nullptr, false },
    /* AtlasCache         */ { "ARLAND_ATLAS_CACHE", nullptr, nullptr, false },
    /* FrameAtlasCache    */ { "ARLAND_FRAME_ATLAS_CACHE", nullptr, nullptr, false },
    /* ResolutionOverride */ { nullptr, nullptr, nullptr, false },
    /* ShadowMultiplier   */ { nullptr, nullptr, nullptr, false },
    /* BattleShadows      */ { "ARLAND_BATTLE_SHADOWS", nullptr, nullptr, false },
    /* CutInShadows       */ { "ARLAND_CUTIN_SHADOWS", "Battle", "BattleCutInShadows", false },
    /* CutInDimHold       */ { "ARLAND_CUTIN_DIMMING", "Battle", "BattleCutInDimming", true },
    /* SkipStartupLogos   */ { "ARLAND_SKIP_LOGOS", "Startup", "SkipLogos", false },
    /* SkipIntroMovie     */ { "ARLAND_SKIP_INTRO_MOVIE", "Startup", "SkipIntroMovie", false },
    /* SynthesisAnimationRate */ { "ARLAND_SYNTH_RATE", nullptr, nullptr, false },
    /* FieldMonsterSnap   */ { "ARLAND_MONSTER_SNAP", "Field", "MonsterSnapFix", false },
    /* FieldCharacterPull */ { "ARLAND_CHARACTER_PULL", "Field", "CharacterPullFix", false },
    /* FastSaveMenu       */ { "ARLAND_SAVE_MENU_GATES", "Menus", "FastSaveMenu", false },
    // No ini key: a correction is not a setting. See pad_rescan.h.
    /* PadRescanBackoff   */ { "ARLAND_PAD_RESCAN", nullptr, nullptr, false },
  };
  return table[static_cast<int>(f)];
}

constexpr Support U = Support::Unsupported;
constexpr Support O = Support::OptIn;
constexpr Support X = Support::OnByDefault;

// The capability matrix. Rows are Rorona / Totori / Meruru, columns follow the
// Feature enum, and a cell is X on by default, O opt-in, U unsupported.
//
// KEEP IN SYNC with the fix and graphics-enhancement tables in README.md, for
// the cells they list. Being user-facing they carry no row for the internal
// cache lifetimes, AtlasCache and FrameAtlasCache.
//
// A cell says whether this game can have the feature and what it defaults to.
// Why it defaults that way belongs beside the code that answers it, in the
// feature's own header or the top of the file that implements it.
//
// TWO CELLS DO NOT MEAN WHAT THE OTHERS MEAN, and both will mislead a reader
// who assumes the table decides behaviour:
//
//   ShadowMultiplier is documentation only. It is a valued knob rather than a
//   switch and has its own reader, so nothing resolves through this cell. It
//   says OnByDefault because the shipped [Rendering] ShadowMultiplier is 2.
//
//   BattleShadows is read directly in battle_shadow_restore.cpp rather than
//   through featureEnabled, so ARLAND_BATTLE_SHADOWS=0 reaches even the rows
//   marked Unsupported. On Totori and Meruru it also stands down the per-frame
//   battle ticks the cut-in gate and dim depend on, and the Present hook that
//   drives them.
//
// BattleCutInDimming is an inverse key: true is the game's original dimming.
// See the descriptor above.
constexpr Support kMatrix[3][static_cast<int>(Feature::Count)] = {
  //           Sync Menu Atls Frme Res  ShMl Bat  CutS CutD Logo Movi Card Snap Pull Save Pad
  /* Rorona */ { X,   X,   X,   X,   X,   X,   X,   O,   O,   O,   O,   X,   X,   X,   X,   X },
  /* Totori */ { X,   X,   X,   X,   X,   X,   U,   O,   O,   O,   O,   X,   X,   X,   X,   X },
  /* Meruru */ { X,   X,   X,   O,   X,   X,   U,   O,   O,   O,   O,   X,   X,   X,   X,   X },
};

int titleRow(Title t) {
  switch (t) {
    case Title::Rorona: return 0;
    case Title::Totori: return 1;
    case Title::Meruru: return 2;
    default: return -1;
  }
}

}  // namespace

Title currentTitle() {
  static const Title title = detectTitle();
  return title;
}

const char* titleName(Title t) {
  switch (t) {
    case Title::Rorona: return "Rorona";
    case Title::Totori: return "Totori";
    case Title::Meruru: return "Meruru";
    default: return "Unknown";
  }
}

Support featureSupport(Feature f) {
  const int row = titleRow(currentTitle());
  if (row < 0)
    return Support::Unsupported;
  return kMatrix[row][static_cast<int>(f)];
}

bool featureEnabled(Feature f) {
  const Support support = featureSupport(f);
  if (support == Support::Unsupported)
    return false;
  const Descriptor& d = descriptor(f);
  const bool actionableDefault = support == Support::OnByDefault;
  if (d.env) {
    if (const char* v = std::getenv(d.env)) {
      const bool user = v[0] != '0';
      return d.invert ? !user : user;
    }
  }
  if (d.section && d.key) {
    const bool keyDefault = d.invert ? !actionableDefault : actionableDefault;
    const bool user = arlandConfigBool(d.section, d.key, keyDefault);
    return d.invert ? !user : user;
  }
  return actionableDefault;
}

}  // namespace atfix
