// SPDX-License-Identifier: MIT
#pragma once

// The replacement UI fonts compiled into the DLL, so "replaced" mode works with
// no loose .ttf beside it. One is bundled per game, chosen by the gameFont()
// matrix in font_hires.cpp: National Park SemiBold (Rorona), Nunito (Totori), and
// Cosmetica Medium (Meruru). A arland-hires-font.ttf next to the DLL, if
// present, overrides them (see loadFont in font_hires.cpp). Each font's license
// is in licenses/.
namespace atfix {
extern const unsigned char kEmbeddedFontNationalParkSemiBold[];
extern const unsigned int kEmbeddedFontNationalParkSemiBoldSize;
extern const unsigned char kEmbeddedFontNunito[];
extern const unsigned int kEmbeddedFontNunitoSize;
extern const unsigned char kEmbeddedFontMgOpenCosmetica[];
extern const unsigned int kEmbeddedFontMgOpenCosmeticaSize;
}  // namespace atfix
