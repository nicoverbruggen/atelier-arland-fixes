// SPDX-License-Identifier: MIT
#pragma once

// The replacement UI font (Cuprum 500) compiled into the DLL, so "replaced" mode
// works with no loose .ttf beside it. A arland-hires-font.ttf next to the DLL, if
// present, overrides this (see loadFont in font_hires.cpp).
namespace atfix {
extern const unsigned char kEmbeddedFont[];
extern const unsigned int kEmbeddedFontSize;
}  // namespace atfix
