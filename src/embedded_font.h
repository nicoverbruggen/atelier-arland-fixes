// SPDX-License-Identifier: MIT
#pragma once

// The replacement UI fonts compiled into the DLL, so "replaced" mode works with
// no loose .ttf beside it. Two are bundled (both OFL); [Rendering] FontName picks
// which -- "NationalPark" (the default) or "Cuprum". A arland-hires-font.ttf next
// to the DLL, if present, overrides both (see loadFont in font_hires.cpp).
namespace atfix {
extern const unsigned char kEmbeddedFontNationalPark[];
extern const unsigned int kEmbeddedFontNationalParkSize;
extern const unsigned char kEmbeddedFontCuprum[];
extern const unsigned int kEmbeddedFontCuprumSize;
}  // namespace atfix
