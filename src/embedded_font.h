// SPDX-License-Identifier: MIT
#pragma once

// The replacement UI fonts compiled into the DLL, so "replaced" mode works with
// no loose .ttf beside it. One is bundled per game, chosen by the gameFont()
// matrix in font_hires.cpp: National Park SemiBold (Rorona), Nunito (Totori), and
// Cosmetica Medium (Meruru). A arland-hires-font.ttf next to the DLL, if
// present, overrides them (see loadFont in font_hires.cpp). Each font's license
// is in licenses/.
//
// Arland Fallback is not one of those: it is the face any of them falls back to
// for a single glyph they do not carry (arrows, shapes, music, Greek), so one
// such character no longer demotes its whole string to the game's baked art. Its
// outlines come from two typefaces, one per family; see licenses/. It
// applies to a user-supplied arland-hires-font.ttf as well.
namespace atfix {
extern const unsigned char kEmbeddedFontNationalParkSemiBold[];
extern const unsigned int kEmbeddedFontNationalParkSemiBoldSize;
extern const unsigned char kEmbeddedFontNunito[];
extern const unsigned int kEmbeddedFontNunitoSize;
extern const unsigned char kEmbeddedFontMgOpenCosmetica[];
extern const unsigned int kEmbeddedFontMgOpenCosmeticaSize;
extern const unsigned char kEmbeddedFontFallback[];
extern const unsigned int kEmbeddedFontFallbackSize;
}  // namespace atfix
