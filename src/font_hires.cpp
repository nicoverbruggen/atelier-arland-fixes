// SPDX-License-Identifier: MIT
//
// High-resolution UI text — "upscaled" mode. For each string the engine renders,
// this upscales the engine's own baked bitmap kScale× (bilinear, or Lanczos with
// ARLAND_HIRES_FILTER=lanczos) with an unsharp-mask sharpen, and substitutes it
// back into the output object. The engine's exact layout is preserved (multi-line,
// alignment, icon glyphs) — only the resolution increases. A per-string result
// cache keeps it off the hot menu path. A "replaced" mode (re-render each string
// from a bundled scalable font) is planned but not implemented here; see TODO.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../vendor/stb/stb_truetype.h"

#include "config.h"
#include "embedded_font.h"
#include "font_hires.h"
#include "game.h"
#include "log.h"

namespace atfix {

extern Log log;   // lives in main.cpp

namespace {

constexpr int kScale = 2;                 // hi-res multiplier
constexpr int kMaxBytes = 32 * 1024 * 1024;

// Stash so the consumer wrapper can restore the pre-double dims (see the header).
thread_local BYTE* g_restoreOutput = nullptr;
thread_local int32_t g_restoreWidth = 0;
thread_local int32_t g_restoreHeight = 0;

// Bilinear upscale of an 8bpp buffer by an integer factor. Smooths the baked
// glyphs' hard edges: the engine then samples this denser bitmap onto the same
// on-screen box, so the text keeps its exact typeface but reads smoother.
void bilinearUpscale(const unsigned char* src, int sw, int sh,
                     unsigned char* dst, int dw, int dh, int factor) {
  const auto clampi = [](int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
  for (int y = 0; y < dh; ++y) {
    const float sy = (y + 0.5f) / factor - 0.5f;
    const int yi = static_cast<int>(std::floor(sy));
    const float fy = sy - yi;
    const int y0 = clampi(yi, sh - 1), y1 = clampi(yi + 1, sh - 1);
    for (int x = 0; x < dw; ++x) {
      const float sx = (x + 0.5f) / factor - 0.5f;
      const int xi = static_cast<int>(std::floor(sx));
      const float fx = sx - xi;
      const int x0 = clampi(xi, sw - 1), x1 = clampi(xi + 1, sw - 1);
      const float a = src[y0 * sw + x0], b = src[y0 * sw + x1];
      const float c = src[y1 * sw + x0], d = src[y1 * sw + x1];
      const float top = a + (b - a) * fx;
      const float bot = c + (d - c) * fx;
      dst[y * dw + x] = static_cast<unsigned char>(top + (bot - top) * fy + 0.5f);
    }
  }
}

// Lanczos-2 separable upscale — a sharper, less-blurry resample than bilinear
// (windowed sinc). Can slightly ring on high-contrast edges; the result is
// clamped. Opt-in via ARLAND_HIRES_FILTER=lanczos (heavier than bilinear).
float sincf(float x) {
  if (x == 0.0f) return 1.0f;
  x *= 3.14159265f;
  return std::sin(x) / x;
}
float lanczos2(float x) {
  if (x < 0.0f) x = -x;
  if (x >= 2.0f) return 0.0f;
  return sincf(x) * sincf(x * 0.5f);
}
void lanczosUpscale(const unsigned char* src, int sw, int sh,
                    unsigned char* dst, int dw, int dh, int factor) {
  const auto clampi = [](int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
  std::vector<float> temp(static_cast<size_t>(dw) * sh);   // horizontal pass
  for (int y = 0; y < sh; ++y) {
    for (int x = 0; x < dw; ++x) {
      const float sx = (x + 0.5f) / factor - 0.5f;
      const int i0 = static_cast<int>(std::floor(sx - 2.0f)) + 1;
      const int i1 = static_cast<int>(std::floor(sx + 2.0f));
      float acc = 0.0f, wsum = 0.0f;
      for (int i = i0; i <= i1; ++i) {
        const float w = lanczos2(sx - i);
        acc += src[y * sw + clampi(i, sw - 1)] * w;
        wsum += w;
      }
      temp[static_cast<size_t>(y) * dw + x] = wsum != 0.0f ? acc / wsum : 0.0f;
    }
  }
  for (int y = 0; y < dh; ++y) {                            // vertical pass
    const float sy = (y + 0.5f) / factor - 0.5f;
    const int j0 = static_cast<int>(std::floor(sy - 2.0f)) + 1;
    const int j1 = static_cast<int>(std::floor(sy + 2.0f));
    for (int x = 0; x < dw; ++x) {
      float acc = 0.0f, wsum = 0.0f;
      for (int j = j0; j <= j1; ++j) {
        const float w = lanczos2(sy - j);
        acc += temp[static_cast<size_t>(clampi(j, sh - 1)) * dw + x] * w;
        wsum += w;
      }
      float v = wsum != 0.0f ? acc / wsum : 0.0f;
      v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
      dst[y * dw + x] = static_cast<unsigned char>(v + 0.5f);
    }
  }
}

// Unsharp-mask sharpen (in place): out = in + amount*(in - blur), 3x3 gaussian
// blur. Restores edge definition softened by the upscale.
void unsharpSharpen(unsigned char* buf, int w, int h, float amount) {
  if (amount <= 0.0f || w < 3 || h < 3)
    return;
  std::vector<unsigned char> blur(static_cast<size_t>(w) * h);
  static const int k[3] = { 1, 2, 1 };
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int sum = 0, wsum = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        const int yy = y + dy;
        if (yy < 0 || yy >= h) continue;
        for (int dx = -1; dx <= 1; ++dx) {
          const int xx = x + dx;
          if (xx < 0 || xx >= w) continue;
          const int wt = k[dy + 1] * k[dx + 1];
          sum += buf[yy * w + xx] * wt;
          wsum += wt;
        }
      }
      blur[static_cast<size_t>(y) * w + x] =
        static_cast<unsigned char>(sum / (wsum ? wsum : 1));
    }
  }
  for (int i = 0; i < w * h; ++i) {
    float v = buf[i] + amount * (buf[i] - blur[i]);
    v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
    buf[i] = static_cast<unsigned char>(v + 0.5f);
  }
}

// Pseudo-SDF edge steepen: a glyph bitmap is an alpha-COVERAGE mask, so its edges
// are an implicit distance field. After a smooth upscale the 0→255 ramp across an
// edge is a few px wide (soft); this contrast-stretches the coverage around the
// 50% midpoint (127.5) to re-crisp the edge while KEEPING it anti-aliased — the
// transition band stays ~255/strength alpha levels wide. Cheap, per-pixel.
void alphaSteepen(unsigned char* buf, int w, int h, float strength) {
  if (strength <= 1.0f)
    return;
  for (int i = 0; i < w * h; ++i) {
    float v = (buf[i] - 127.5f) * strength + 127.5f;
    v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
    buf[i] = static_cast<unsigned char>(v + 0.5f);
  }
}

// Upscale filter: ARLAND_HIRES_FILTER = "sdf" (default: bilinear + coverage
// steepen), "bilinear" (+ unsharp), or "lanczos" (+ unsharp).
enum class Filter { Bilinear, Lanczos, Sdf };
Filter filterMode() {
  static const Filter f = [] {
    const char* v = std::getenv("ARLAND_HIRES_FILTER");
    if (v) {
      if (v[0] == 'l' || v[0] == 'L') return Filter::Lanczos;
      if (v[0] == 'b' || v[0] == 'B') return Filter::Bilinear;
      if (v[0] == 's' || v[0] == 'S') return Filter::Sdf;
    }
    return Filter::Sdf;
  }();
  return f;
}
float sdfStrength() {
  static const float s = [] {
    const char* v = std::getenv("ARLAND_HIRES_SDF");
    const float x = v ? static_cast<float>(std::atof(v)) : 3.0f;
    return (x >= 1.0f && x <= 16.0f) ? x : 3.0f;
  }();
  return s;
}
float sharpenAmount() {
  static const float amount = [] {
    const char* v = std::getenv("ARLAND_HIRES_SHARPEN");
    const float s = v ? static_cast<float>(std::atof(v)) : 0.5f;   // mild default
    return (s >= 0.0f && s <= 4.0f) ? s : 0.5f;
  }();
  return amount;
}

// Result cache: (string + size + font state) -> the finished upscaled+sharpened
// bytes. renderText is the hot menu path and re-renders the same strings
// constantly (per frame, on scroll, on re-open), so caching turns the filter
// cost into a one-time-per-string memcpy. Keyed by content so a changed string
// recomputes; capped and cleared wholesale on overflow.
std::mutex g_cacheMutex;
std::unordered_map<uint64_t, std::vector<unsigned char>> g_upscaleCache;
constexpr size_t kCacheMaxEntries = 2048;

uint64_t upscaleKey(const char* utf8, uint32_t a, uint32_t b,
                    uint32_t c, uint32_t d) {
  uint64_t k = 0xcbf29ce484222325ULL;
  const auto mix = [&](uint64_t v) { k ^= v; k *= 0x100000001b3ULL; };
  for (auto* p = reinterpret_cast<const unsigned char*>(utf8); *p; ++p)
    mix(*p);
  mix(a); mix(b); mix(c); mix(d);
  return k;
}

// Build a path to `filename` in the directory containing this DLL.
bool moduleFilePath(const char* filename, char* out) {
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&moduleFilePath), &self))
    return false;
  const DWORD n = GetModuleFileNameA(self, out, MAX_PATH);
  if (!n || n >= MAX_PATH)
    return false;
  char* back = std::strrchr(out, '\\');
  char* forward = std::strrchr(out, '/');
  char* sep = back > forward ? back : forward;
  if (!sep)
    return false;
  std::strcpy(sep + 1, filename);
  return true;
}

// Write an 8bpp buffer beside the DLL as a binary PGM, for offline inspection of
// the original baked bitmap vs the upscaled one (ARLAND_HIRES_DUMP only).
void writePgm(const char* filename, const unsigned char* data, int w, int h) {
  char path[MAX_PATH] = { };
  if (w <= 0 || h <= 0 || !data || !moduleFilePath(filename, path))
    return;
  HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  char header[64] = { };
  const int headerLen = wsprintfA(header, "P5\n%d %d\n255\n", w, h);
  DWORD written = 0;
  WriteFile(file, header, DWORD(headerLen), &written, nullptr);
  WriteFile(file, data, DWORD(w) * DWORD(h), &written, nullptr);
  CloseHandle(file);
}

// One-shot dump of the original + upscaled bitmap for the first string matching
// ARLAND_HIRES_DUMP (its value is a case-insensitive substring; "1" ⇒ "totori").
std::atomic<bool> g_dumped = { false };
void maybeDump(const char* utf8, const unsigned char* orig, int ow, int oh,
               const unsigned char* hires, int hw, int hh) {
  const char* want = std::getenv("ARLAND_HIRES_DUMP");
  if (!want || want[0] == '0')
    return;
  const char* target = (want[0] == '1' && want[1] == '\0') ? "totori" : want;
  char needle[64] = { };
  char hay[256] = { };
  for (size_t i = 0; target[i] && i + 1 < sizeof(needle); ++i)
    needle[i] = (target[i] >= 'A' && target[i] <= 'Z') ? char(target[i] + 32)
                                                       : target[i];
  for (size_t i = 0; utf8[i] && i + 1 < sizeof(hay); ++i)
    hay[i] = (utf8[i] >= 'A' && utf8[i] <= 'Z') ? char(utf8[i] + 32) : utf8[i];
  if (!std::strstr(hay, needle) || g_dumped.exchange(true))
    return;
  writePgm("arland-hires-orig.pgm", orig, ow, oh);
  writePgm("arland-hires-hi.pgm", hires, hw, hh);
  log("HiResText: dumped comparison for \"", utf8, "\" orig=", std::dec, ow,
    "x", oh, " hi=", hw, "x", hh);
}

// ---- "replaced" mode: re-render each string from a bundled scalable font -----

std::atomic<bool> g_triedInit = { false };
bool g_ready = false;
std::vector<unsigned char> g_ttf;
stbtt_fontinfo g_font;

// Load arland-hires-font.ttf from the module directory (beside d3d11.dll) if the
// user dropped one there to override the embedded font. Absent = normal (returns
// false quietly so loadFont falls back to the embedded font).
bool loadFontFile() {
  char path[MAX_PATH] = { };
  HMODULE self = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&loadFontFile), &self))
    return false;
  const DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
  if (!n || n >= MAX_PATH)
    return false;
  char* back = std::strrchr(path, '\\');
  char* forward = std::strrchr(path, '/');
  char* sep = back > forward ? back : forward;
  if (!sep)
    return false;
  std::strcpy(sep + 1, "arland-hires-font.ttf");
  HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return false;                 // no override present -> use the embedded font
  LARGE_INTEGER size = { };
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
      size.QuadPart > kMaxBytes) {
    CloseHandle(file);
    return false;
  }
  g_ttf.resize(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ReadFile(file, g_ttf.data(),
    static_cast<DWORD>(g_ttf.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok || read != g_ttf.size())
    return false;
  if (!stbtt_InitFont(&g_font, g_ttf.data(),
        stbtt_GetFontOffsetForIndex(g_ttf.data(), 0)))
    return false;
  log("HiResText: loaded arland-hires-font.ttf override (", std::dec,
    static_cast<unsigned>(g_ttf.size()), " bytes)");
  return true;
}

// Prefer a user override beside the DLL; otherwise use the font compiled into the
// DLL (Cuprum 500) so "replaced" mode needs no loose .ttf. The embedded array has
// program lifetime, so stbtt can point straight at it without a copy.
bool loadFont() {
  if (loadFontFile())
    return true;
  if (!stbtt_InitFont(&g_font, kEmbeddedFont,
        stbtt_GetFontOffsetForIndex(kEmbeddedFont, 0)))
    return false;
  log("HiResText: using embedded font (", std::dec, kEmbeddedFontSize,
    " bytes)");
  return true;
}
void ensureInit() {
  if (g_triedInit.exchange(true, std::memory_order_acq_rel))
    return;
  g_ready = loadFont();
}

// Decode UTF-8 to codepoints, KEEPING '\n' as a line break; CR/TAB -> space.
void decodeUtf8Lines(const char* s, std::vector<int>& out) {
  for (size_t i = 0; s[i] && i < 8192; ) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int cp = c, extra = 0;
    if (c >= 0xF0) { cp = c & 0x07; extra = 3; }
    else if (c >= 0xE0) { cp = c & 0x0F; extra = 2; }
    else if (c >= 0xC0) { cp = c & 0x1F; extra = 1; }
    ++i;
    for (int k = 0; k < extra && s[i]; ++k, ++i)
      cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    // Fold stray whitespace (incl. full-width U+3000, nbsp, thin/zero-width) to a
    // normal space so it doesn't trip the missing-glyph "keep baked" guard.
    if (cp == '\r' || cp == '\t' || cp == 0x00A0 || cp == 0x3000 ||
        cp == 0x202F || cp == 0x205F || cp == 0xFEFF ||
        (cp >= 0x2000 && cp <= 0x200B))
      cp = ' ';
    // Fold the full-width ASCII block (U+FF01..FF5E) to plain ASCII — the game
    // renders dates/numbers with full-width digits (１２３ etc.), which a Latin
    // replacement font otherwise can't resolve.
    else if (cp >= 0xFF01 && cp <= 0xFF5E)
      cp -= 0xFEE0;
    // Map the CJK punctuation the game uses in EN labels to ASCII equivalents so
    // a Latin font can render them (e.g. the label brackets 【Effect】 -> [Effect]).
    else if (cp == 0x3010 || cp == 0x300C || cp == 0x300E) cp = '[';  // 【「『
    else if (cp == 0x3011 || cp == 0x300D || cp == 0x300F) cp = ']';  // 】」』
    else if (cp == 0x3001) cp = ',';                                  // 、
    else if (cp == 0x3002) cp = '.';                                  // 。
    out.push_back(cp);
  }
}

// Glyph atlas cache: rasterize each (glyph, size) once, then compose strings by
// blitting cached glyphs (fast — the "cache the alphabet" idea).
struct CachedGlyph {
  std::vector<unsigned char> bitmap;
  int w = 0, h = 0, xoff = 0, yoff = 0;
  float advance = 0.0f;
};
std::mutex g_glyphMutex;
std::unordered_map<uint64_t, CachedGlyph> g_glyphCache;

// Blit codepoint `cp` at pen (penX, baseline); returns its advance (px). Holds
// the glyph lock (rasterizes on first use for that size, then it's a memcpy).
float drawGlyph(unsigned char* dst, int dw, int dh, int penX, int baseline,
                int cp, float scale, int sizeKey) {
  std::lock_guard<std::mutex> lock(g_glyphMutex);
  const uint64_t key =
    (static_cast<uint64_t>(static_cast<uint32_t>(cp)) << 32) |
    static_cast<uint32_t>(sizeKey);
  auto it = g_glyphCache.find(key);
  if (it == g_glyphCache.end()) {
    CachedGlyph g;
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&g_font, cp, &adv, &lsb);
    g.advance = adv * scale;
    int w = 0, h = 0, xo = 0, yo = 0;
    unsigned char* bmp =
      stbtt_GetCodepointBitmap(&g_font, scale, scale, cp, &w, &h, &xo, &yo);
    if (bmp) {
      g.w = w; g.h = h; g.xoff = xo; g.yoff = yo;
      g.bitmap.assign(bmp, bmp + static_cast<size_t>(w) * h);
      stbtt_FreeBitmap(bmp, nullptr);
    }
    it = g_glyphCache.emplace(key, std::move(g)).first;
  }
  const CachedGlyph& g = it->second;
  for (int y = 0; y < g.h; ++y) {
    const int dy = baseline + g.yoff + y;
    if (dy < 0 || dy >= dh) continue;
    for (int x = 0; x < g.w; ++x) {
      const int dx = penX + g.xoff + x;
      if (dx < 0 || dx >= dw) continue;
      const unsigned char v = g.bitmap[static_cast<size_t>(y) * g.w + x];
      unsigned char& d = dst[static_cast<size_t>(dy) * dw + dx];
      if (v > d) d = v;
    }
  }
  return g.advance;
}

// Replacement-font size multiplier / vertical nudge (compensate per-font metrics).
float userScale() {
  static const float s = [] {
    // Default below 1.0: scaling the font to the full line height renders it
    // larger than the baked glyphs (which don't fill their 48px cell), so trim
    // it to sit at roughly the original on-screen size.
    const char* v = std::getenv("ARLAND_HIRES_SCALE");
    const float x = v ? static_cast<float>(std::atof(v)) : 0.85f;
    return (x >= 0.3f && x <= 2.0f) ? x : 0.85f;
  }();
  return s;
}
int userVOff() {
  static const int v = [] {
    const char* e = std::getenv("ARLAND_HIRES_VOFF");
    return e ? std::atoi(e) : 0;
  }();
  return v;
}

// Re-render the string from the bundled font and substitute it. Replicates the
// engine's layout (RE of renderText 0x430bf0): split on '\n' only (no wrap),
// stack lines by lineHeight, each left-aligned from x=0, ink centered in its
// line slot; usedW = max line width. Glyphs come from the atlas cache. Returns
// false (keeps the baked bitmap) if the font can't render every glyph.
bool renderReplaced(BYTE* output, const char* utf8, uintptr_t pixels,
                    int width, int height, int newW, int newH,
                    const float metrics[4], HiResAllocFn alloc,
                    HiResFreeFn free) {
  const float usedHFrac = metrics[1];   // usedH / potH (all lines)
  if (!(usedHFrac > 0.0f && usedHFrac <= 1.0f))
    return false;
  const int usedH = static_cast<int>(usedHFrac * newH + 0.5f);
  if (usedH <= 0)
    return false;

  std::vector<int> cps;
  cps.reserve(96);
  decodeUtf8Lines(utf8, cps);
  if (cps.empty())
    return false;
  for (const int cp : cps)
    if (cp != '\n' && cp != ' ' && stbtt_FindGlyphIndex(&g_font, cp) == 0) {
      // The bundled font has no glyph for this codepoint (e.g. the game's custom
      // button-prompt icons). Bail so the caller upscales the baked bitmap of the
      // whole string instead — the icon stays intact, the rest still smooths.
      if (verboseLogging())
        log("HiResText: font lacks U+", std::hex, cp, std::dec,
          " -> upscale \"", utf8, "\"");
      return false;
    }

  int numLines = 1;
  for (const int cp : cps)
    if (cp == '\n') ++numLines;
  const float lineH = static_cast<float>(usedH) / numLines;
  const float scale = stbtt_ScaleForPixelHeight(&g_font, lineH) * userScale();
  const int sizeKey = static_cast<int>(scale * 8192.0f + 0.5f);

  // Fixed baseline reference: center the cap box (measured once from 'H') in the
  // line slot. Using a font constant instead of each string's own ink keeps every
  // row on the SAME baseline -> even menu rhythm (a string with descenders no
  // longer rides higher than an all-caps one); capitals stay optically centered.
  int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
  stbtt_GetCodepointBitmapBox(&g_font, 'H', scale, scale, &hx0, &hy0, &hx1, &hy1);
  const int baseInLine = static_cast<int>(lineH * 0.5f - (hy0 + hy1) * 0.5f);

  void* buffer = alloc(static_cast<size_t>(newW) * newH);
  if (!buffer)
    return false;
  std::memset(buffer, 0, static_cast<size_t>(newW) * newH);
  auto* dst = reinterpret_cast<unsigned char*>(buffer);

  float maxW = 0.0f;
  int lineIdx = 0;
  size_t i = 0;
  while (i < cps.size()) {
    size_t j = i;
    while (j < cps.size() && cps[j] != '\n') ++j;
    const int lineTop = static_cast<int>(lineIdx * lineH + 0.5f);
    const int baseline = lineTop + baseInLine + userVOff() * kScale;
    float penX = 0.0f;
    for (size_t g = i; g < j; ++g) {
      penX += drawGlyph(dst, newW, newH, static_cast<int>(penX + 0.5f),
        baseline, cps[g], scale, sizeKey);
      if (g + 1 < j)
        penX += stbtt_GetCodepointKernAdvance(&g_font, cps[g], cps[g + 1]) *
          scale;
    }
    if (penX > maxW) maxW = penX;
    ++lineIdx;
    i = (j < cps.size()) ? j + 1 : j;
  }
  int renderedW = static_cast<int>(maxW + 0.5f);
  if (renderedW > newW) renderedW = newW;
  if (renderedW < 1) renderedW = 1;

  maybeDump(utf8, reinterpret_cast<const unsigned char*>(pixels), width, height,
    dst, newW, newH);
  free(reinterpret_cast<void*>(pixels));
  const uintptr_t newPixels = reinterpret_cast<uintptr_t>(buffer);
  std::memcpy(output + 8, &newPixels, sizeof(newPixels));
  std::memcpy(output, &newW, sizeof(newW));
  std::memcpy(output + 4, &newH, sizeof(newH));
  // Width fraction = natural text width (keeps the font's real spacing); height
  // fraction, dead field and numLines are left as the engine set them.
  const float fracU = static_cast<float>(renderedW) / static_cast<float>(newW);
  std::memcpy(output + 0x10, &fracU, sizeof(fracU));
  g_restoreOutput = output;
  g_restoreWidth = width;
  g_restoreHeight = height;
  return true;
}

// "upscaled" mode: smooth the baked bitmap and substitute it. Content and layout
// (multi-line, alignment, icons) are the engine's own, so only the resolution
// changes; all metrics are kept, only the pow2 dims and the pixel buffer grow.
// Also the fallback when "replaced" can't render a string.
bool renderUpscaled(BYTE* output, const char* utf8, uintptr_t pixels,
                    int width, int height, int newW, int newH,
                    uintptr_t renderer, HiResAllocFn alloc, HiResFreeFn free) {
  void* buffer = alloc(static_cast<size_t>(newW) * newH);
  if (!buffer)
    return false;
  const auto* src = reinterpret_cast<const unsigned char*>(pixels);
  auto* out = reinterpret_cast<unsigned char*>(buffer);
  const size_t bytes = static_cast<size_t>(newW) * newH;

  // Cache on (string, size, font state) so a repeat render is a plain memcpy
  // instead of re-running the filter (renderText is the hot menu path).
  const auto* r = reinterpret_cast<const BYTE*>(renderer);
  uint32_t modeField = 0;
  std::memcpy(&modeField, r + 0x1cc, sizeof(modeField));
  const uint64_t key = upscaleKey(utf8, static_cast<uint32_t>(width),
    static_cast<uint32_t>(height), r[0x1c8], modeField);
  bool hit = false;
  {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    const auto it = g_upscaleCache.find(key);
    if (it != g_upscaleCache.end() && it->second.size() == bytes) {
      std::memcpy(out, it->second.data(), bytes);
      hit = true;
    }
  }
  if (!hit) {
    switch (filterMode()) {
      case Filter::Lanczos:
        lanczosUpscale(src, width, height, out, newW, newH, kScale);
        unsharpSharpen(out, newW, newH, sharpenAmount());
        break;
      case Filter::Sdf:
        bilinearUpscale(src, width, height, out, newW, newH, kScale);
        alphaSteepen(out, newW, newH, sdfStrength());
        break;
      case Filter::Bilinear:
      default:
        bilinearUpscale(src, width, height, out, newW, newH, kScale);
        unsharpSharpen(out, newW, newH, sharpenAmount());
        break;
    }
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    if (g_upscaleCache.size() >= kCacheMaxEntries)
      g_upscaleCache.clear();
    g_upscaleCache.emplace(key, std::vector<unsigned char>(out, out + bytes));
  }
  maybeDump(utf8, src, width, height, out, newW, newH);

  free(reinterpret_cast<void*>(pixels));
  const uintptr_t newPixels = reinterpret_cast<uintptr_t>(buffer);
  std::memcpy(output + 8, &newPixels, sizeof(newPixels));
  std::memcpy(output, &newW, sizeof(newW));
  std::memcpy(output + 4, &newH, sizeof(newH));
  // output+0x20 (numLines) is left as the engine set it; not a "ready" flag.
  // The consumer wrapper needs the doubled dims to build its texture; stash the
  // originals so it can restore them afterward (keeps auto-size widgets' size).
  g_restoreOutput = output;
  g_restoreWidth = width;
  g_restoreHeight = height;
  return true;
}

}  // namespace

bool hiResTextEnabled() {
  // Active in "upscaled" (smooth the baked glyphs) and "replaced" (re-render from
  // the bundled font) modes, on any recognized Arland title. The per-game text
  // allocator and consumer-restore hook are resolved for the English builds only
  // (installTextBitmapAllocator / installHiResTextConsumer); on multilingual
  // builds those stay unresolved so the substitution safely no-ops.
  static const bool on = [] {
    const UIFontMode m = uiFontMode();
    return (m == UIFontMode::Upscaled || m == UIFontMode::Replaced) &&
           currentTitle() != Title::Unknown;
  }();
  return on;
}

bool hiResTextRerender(uintptr_t renderer, const char* utf8,
                       HiResAllocFn alloc, HiResFreeFn free) {
  static std::atomic<bool> logged = { false };
  if (!logged.exchange(true, std::memory_order_relaxed))
    log("HiResText: mode=", static_cast<int>(uiFontMode()),
      " enabled=", hiResTextEnabled() ? 1 : 0,
      " title=", static_cast<int>(currentTitle()),
      " alloc=", alloc ? 1 : 0, " free=", free ? 1 : 0);
  if (!hiResTextEnabled() || !renderer || !utf8 || !alloc || !free)
    return false;
  bool fontReady = false;
  if (uiFontMode() == UIFontMode::Replaced) {
    ensureInit();
    fontReady = g_ready;         // no bundled font -> fall back to upscaling below
  }

  // The engine stashes the finished per-string output object at renderer+0x1a0:
  // [0]=int32 potW, [4]=int32 potH, [8]=ptr to potW*potH 8bpp alpha,
  // [0x10]=four normalized metrics (used/pot fractions), [0x20]=numLines.
  auto* output = *reinterpret_cast<BYTE**>(
    reinterpret_cast<BYTE*>(renderer) + 0x1a0);
  if (!output)
    return false;
  int32_t width = 0, height = 0;
  uintptr_t pixels = 0;
  float metrics[4] = { };
  std::memcpy(&width, output, sizeof(width));
  std::memcpy(&height, output + 4, sizeof(height));
  std::memcpy(&pixels, output + 8, sizeof(pixels));
  std::memcpy(metrics, output + 0x10, sizeof(metrics));
  if (width <= 0 || height <= 0 || !pixels)
    return false;
  const int newW = width * kScale;
  const int newH = height * kScale;
  if (static_cast<int64_t>(newW) * newH > kMaxBytes)
    return false;

  // "replaced" is best-effort: re-render from the bundled font when every glyph
  // resolves, otherwise fall through and upscale the baked bitmap so the string
  // is still smoothed (never left as the raw baked art).
  if (fontReady && renderReplaced(output, utf8, pixels, width, height, newW, newH,
        metrics, alloc, free))
    return true;

  return renderUpscaled(output, utf8, pixels, width, height, newW, newH,
    renderer, alloc, free);
}

void hiResTextRestoreDims() {
  if (!g_restoreOutput)
    return;
  std::memcpy(g_restoreOutput, &g_restoreWidth, sizeof(g_restoreWidth));
  std::memcpy(g_restoreOutput + 4, &g_restoreHeight, sizeof(g_restoreHeight));
  g_restoreOutput = nullptr;
}

}  // namespace atfix
