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
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "config.h"
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

}  // namespace

bool hiResTextEnabled() {
  // Active in "upscaled" mode on any recognized Arland title. The per-game text
  // allocator and consumer-restore hook are resolved for the English builds only
  // (installTextBitmapAllocator / installHiResTextConsumer); on multilingual
  // builds those stay unresolved so the substitution safely no-ops.
  static const bool on = [] {
    return uiFontMode() == UIFontMode::Upscaled &&
           currentTitle() != Title::Unknown;
  }();
  return on;
}

bool hiResTextRerender(uintptr_t renderer, const char* utf8,
                       HiResAllocFn alloc, HiResFreeFn free) {
  static std::atomic<bool> logged = { false };
  if (!logged.exchange(true, std::memory_order_relaxed))
    log("HiResText: mode=upscaled enabled=", hiResTextEnabled() ? 1 : 0,
      " title=", static_cast<int>(currentTitle()),
      " alloc=", alloc ? 1 : 0, " free=", free ? 1 : 0);
  if (!hiResTextEnabled() || !renderer || !utf8 || !alloc || !free)
    return false;

  // The engine stashes the finished per-string output object at renderer+0x1a0:
  // [0]=int32 potW, [4]=int32 potH, [8]=ptr to potW*potH 8bpp alpha,
  // [0x10]=four normalized metrics (used/pot fractions), [0x20]=numLines.
  auto* output = *reinterpret_cast<BYTE**>(
    reinterpret_cast<BYTE*>(renderer) + 0x1a0);
  if (!output)
    return false;
  int32_t width = 0, height = 0;
  uintptr_t pixels = 0;
  std::memcpy(&width, output, sizeof(width));
  std::memcpy(&height, output + 4, sizeof(height));
  std::memcpy(&pixels, output + 8, sizeof(pixels));
  if (width <= 0 || height <= 0 || !pixels)
    return false;
  const int newW = width * kScale;
  const int newH = height * kScale;
  if (static_cast<int64_t>(newW) * newH > kMaxBytes)
    return false;

  // Smooth the baked bitmap and substitute it. Content and layout (multi-line,
  // alignment, icons) are the engine's own, so only the resolution changes; all
  // metrics are kept, only the pow2 dims and the pixel buffer grow.
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

void hiResTextRestoreDims() {
  if (!g_restoreOutput)
    return;
  std::memcpy(g_restoreOutput, &g_restoreWidth, sizeof(g_restoreWidth));
  std::memcpy(g_restoreOutput + 4, &g_restoreHeight, sizeof(g_restoreHeight));
  g_restoreOutput = nullptr;
}

}  // namespace atfix
