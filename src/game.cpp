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
// Feature enum. KEEP IN SYNC with the fix and graphics-enhancement tables in
// README.md, for the cells they list: being user-facing, they have no row for
// the internal cache lifetimes (AtlasCache, FrameAtlasCache).
// Notes: ShadowMultiplier is OnByDefault on all three. It is a valued knob
// rather than a switch, read by its own reader, so this cell is documentation
// only and nothing resolves behaviour through it; it says OnByDefault because
// the shipped [Rendering] ShadowMultiplier is 2, not 1.
// FrameAtlasCache is OnByDefault on Rorona and Totori, OptIn on Meruru,
// where it measurably buys nothing: its queue drain already serves all but three
// of the reads, so a longer snapshot lifetime would be exposure without a win.
// BattleShadows (mod-side caster restoration) is OnByDefault only on Rorona;
// Meruru and Totori cast them natively (Totori confirmed healthy by the
// 2026-07-23 probe). It has no ini key: Rorona ships without shadows the engine
// plainly means to draw, so restoring them is a defect correction and not a
// preference. ARLAND_BATTLE_SHADOWS=0 remains, for an A/B during development. It
// is read directly in battle_shadow_restore.cpp rather than through
// featureEnabled, so unlike the other switches it reaches the Unsupported rows:
// on Totori and Meruru setting it to 0 also stands down the per-frame battle
// ticks that the cut-in gate and dim depend on, and the Present hook that drives
// them.
// CutInShadows and CutInDimHold are OptIn on all three games. They change how
// the close-up attack cameras look rather than repair them, and they are still
// being playtested, so they ship off and are selected in the launcher's
// "Attack cut-ins" list ([Battle] BattleCutInShadows / BattleCutInDimming; the
// latter is the inverse key, see the descriptor -- BattleCutInDimming=true is
// the original dimming). Totori's cut-in support covers both builds: its
// multilingual battle addresses were RTTI-located and homologue-matched, then
// confirmed in-game, so the cut-in cells apply to every supported executable.
// The two startup skips are opt-in in all three games. Their hook targets were
// resolved per game rather than ported across games: each logo vtable came from
// that game's own RTTI, and each English movie routine was anchored on that
// build's own path string, with each multilingual address matched from its own
// English build. Every prologue is verified before install.
// Confirmed in game on Rorona; Totori and Meruru are wired but untested.
// SynthesisAnimationRate ships on in all three: it is a defect correction
// rather than a preference, and at 59.94 Hz and below the original runs
// untouched, so a display that never exceeds 60 Hz sees no change at all. The
// same correction is confirmed in game in both KTGL Dusk titles.
// ARLAND_SYNTH_RATE=0 is the A/B switch.
// MonsterSnapFix spreads a monster's re-target correction over time instead of
// applying it in one frame, and only touches charas the game lists as field
// enemies. It covers all six executables: Rorona and Meruru name the family
// properly in RTTI (nspFM::clsFM*) and expose the chara manager's own update as
// a per-frame entry, Totori's two builds hook the field map's subsystem tick,
// and every build's container and node offsets were read from its own
// disassembly rather than ported.
// It ships on in all three games and every language version: it corrects a
// defect in how a monster's movement is delivered rather than expressing a
// preference, and the correction only changes how quickly the monster covers
// ground, never where it ends up or when. CharacterPullFix clamps the
// separation depth at zero; it is a patch to a shared engine routine and so
// reaches every character pair including the player and party, which is why it
// has its own key and can be turned off on its own. It ships on in all three
// games as a defect correction rather than a preference. The arithmetic is not
// in question: the depth is signed, nothing clamps it, and the product is
// applied, so a pair further apart than their combined radii is pulled together
// rather than left alone. It was measured doing exactly that, drawing a monster
// from 0.867 to 0.800. What is not established is whether the effect is
// perceptible, since the pair is only close enough for it to act during the
// moment before an encounter starts. It ships on because the correction is
// strictly subtractive: max(depth, 0) can only remove a pull, never introduce
// or alter a push, so the patched behaviour is a subset of the original's.
// FastSaveMenu removes the hardcoded waits in front of the save data slots
// view: 0.3 s then 0.5 s from the main menu, 1.5 s from inside the Atelier, and
// 1.5 s again on the way out. None of the waits polls I/O or object readiness;
// every condition is pure elapsed time, which is why the view was equally slow
// on a rig where the storage calls are local file operations.
// All three games, both builds each; Rorona and Meruru carry a fifth gate that
// Totori lacks. Confirmed in play on Rorona and Totori English. Reading the code
// suggested Rorona's title wait paced a real fade; playing it showed no fade
// there at all, so that concern is withdrawn.
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
