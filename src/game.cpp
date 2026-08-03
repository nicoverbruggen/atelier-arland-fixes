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
    /* Msaa               */ { nullptr, nullptr, nullptr, false },
    /* ShadowMultiplier   */ { nullptr, nullptr, nullptr, false },
    /* BattleShadows      */ { "ARLAND_BATTLE_SHADOWS", "Battle", "BattleShadows", false },
    /* CutInShadows       */ { "ARLAND_CUTIN_SHADOWS", "Battle", "BattleCutInShadows", false },
    /* CutInDimHold       */ { "ARLAND_CUTIN_DIMMING", "Battle", "BattleCutInDimming", true },
    /* SkipStartupLogos   */ { "ARLAND_SKIP_LOGOS", "Startup", "SkipLogos", false },
    /* SkipIntroMovie     */ { "ARLAND_SKIP_INTRO_MOVIE", "Startup", "SkipIntroMovie", false },
    /* SynthesisAnimationRate */ { "ARLAND_SYNTH_RATE", nullptr, nullptr, false },
  };
  return table[static_cast<int>(f)];
}

constexpr Support U = Support::Unsupported;
constexpr Support O = Support::OptIn;
constexpr Support X = Support::OnByDefault;

// The capability matrix. Rows are Rorona / Totori / Meruru, columns follow the
// Feature enum. KEEP IN SYNC with the "Feature support by game" table in
// README.md, for the cells it lists: being user-facing, it has no row for the
// internal cache lifetimes (AtlasCache, FrameAtlasCache).
// Notes: FrameAtlasCache is OnByDefault on Rorona and Totori, OptIn on Meruru,
// where it measurably buys nothing: its queue drain already serves all but three
// of the reads, so a longer snapshot lifetime would be exposure without a win.
// BattleShadows (mod-side
// caster restoration) is OnByDefault only on Rorona; Meruru and Totori cast
// them natively (Totori confirmed healthy by the 2026-07-23 probe). CutInShadows
// and CutInDimHold ship OnByDefault on all three games: cut-ins keep their
// ground shadows and stay at full brightness. Turn either back off via [Battle]
// BattleCutInShadows / BattleCutInDimming (the latter is the inverse key, see
// the descriptor -- BattleCutInDimming=true restores the original dimming).
// They shipped opt-in until the cut-in character-juggling stray-shadow glitch
// was fixed on 2026-07-23 (the settle-gated reception hold plus the
// force-expiry per-actor hide; validated in Rorona, Meruru, and Totori), and
// are on by default now that the playtest they were waiting on has happened.
// Totori's cut-in support now
// covers both builds: its multilingual battle addresses were RTTI-located and
// homologue-matched, then confirmed in-game, so the cut-in cells apply to every
// supported executable.
// The two startup skips are opt-in in all three games. Their hook targets were
// resolved per game rather than ported by homology -- each logo vtable came
// from that game's own RTTI, and each movie routine was anchored on that
// build's own path string -- and every prologue is verified before install.
// Confirmed in game on Rorona; Totori and Meruru are wired but untested.
// SynthesisAnimationRate ships on in all three: it is a defect correction
// rather than a preference, and at 59.94 Hz and below the original runs
// untouched, so a display that never exceeds 60 Hz sees no change at all. The
// same correction is confirmed in game in both KTGL Dusk titles.
// ARLAND_SYNTH_RATE=0 is the A/B switch.
constexpr Support kMatrix[3][static_cast<int>(Feature::Count)] = {
  //           Sync Menu Atls Frme Res  MSAA ShMl Bat  CutS CutD Logo Movi Card
  /* Rorona */ { X,   X,   X,   X,   X,   O,   O,   X,   X,   X,   O,   O,   X },
  /* Totori */ { X,   X,   X,   X,   X,   O,   O,   U,   X,   X,   O,   O,   X },
  /* Meruru */ { X,   X,   X,   O,   X,   O,   O,   U,   X,   X,   O,   O,   X },
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
