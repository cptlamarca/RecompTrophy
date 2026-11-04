// See screen.h. VideoOut as documented by the PS5 payload SDK's
// ps5platform/videoout.h: two 16 MiB buffers in write-combined direct memory,
// B8G8R8A8 SDR pixels in 64 KiB tiles of 128x128 (tiling 0); the CPU's
// writes flushed from its caches before each flip.

#include "screen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h"

extern "C" {
size_t sceKernelGetDirectMemorySize();
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t length, size_t alignment, int type,
                                  int64_t* physical);
int sceKernelMapDirectMemory(void** address, size_t length, int protection, int flags, int64_t physical,
                             size_t alignment);
int sceVideoOutOpen(int32_t user, int32_t bus, int32_t index, const void* parameter);
int sceVideoOutSetFlipRate(int32_t handle, int32_t rate);
void sceVideoOutSetBufferAttribute2(void* attribute, uint64_t pixel_format, uint32_t tiling, uint32_t width,
                                    uint32_t height, uint64_t reserved1, uint32_t reserved2,
                                    uint64_t reserved3);
int sceVideoOutRegisterBuffers2(int32_t handle, int32_t set, int32_t start, void* buffers, int32_t count,
                                void* attribute, int32_t reserved, void* option);
int sceVideoOutSubmitFlip(int32_t handle, int32_t buffer_index, uint32_t mode, int64_t argument);
int sceVideoOutWaitVblank(int32_t handle);
int sceVideoOutIsFlipPending(int32_t handle);
int sceSystemServiceHideSplashScreen();
}

namespace screen {
namespace {

constexpr size_t kBufferBytes = 0x1000000;
constexpr size_t kAlignment = 0x200000;
constexpr int kMemoryWriteCombined = 3;
constexpr int kProtection = 0x33;  // CPU and GPU, read and write
constexpr uint64_t kPixelFormatBgra8 = 0x8000000000000000ull;

int video = -1;
uint8_t* buffers[2] = {};
int back = 0;
int64_t flips = 0;
std::vector<uint32_t> picture(size_t(kWidth) * kHeight);

// Where pixel (x, y) is in a tiled buffer: 128x128 tiles of 64 KiB, in rows;
// inside one, the bits of x and y interleaved as the display reads them.
uint32_t column_offset[kWidth];
uint32_t row_term[kHeight];
uint32_t row_base[kHeight];

void MakeTiling() {
  const uint32_t tiles_per_row = (kWidth + 127) / 128;
  for (uint32_t x = 0; x < uint32_t(kWidth); ++x) {
    column_offset[x] = ((x >> 7) << 16) | (((x << 2) & 0xc) ^ ((x << 5) & 0x380) ^ ((x << 4) & 0x400) ^
                                           ((x << 6) & 0x800) ^ ((x << 9) & 0xa000));
  }
  for (uint32_t y = 0; y < uint32_t(kHeight); ++y) {
    row_term[y] = ((y << 4) & 0x70) ^ ((y << 5) & 0xf00) ^ ((y << 9) & 0x1000) ^ ((y << 8) & 0x4000);
    row_base[y] = ((y >> 7) * tiles_per_row) << 16;
  }
}

void Flush(void* address, size_t length) {
  for (auto* at = static_cast<uint8_t*>(address); at < static_cast<uint8_t*>(address) + length; at += 64) {
    __asm__ volatile("clflush (%0)" : : "r"(at) : "memory");
  }
  __asm__ volatile("mfence" ::: "memory");
}

// ---- text ------------------------------------------------------------------

struct Face {
  std::vector<uint8_t> data;
  stbtt_fontinfo info{};
  bool ok = false;
};
Face faces[2];

struct Glyph {
  int w = 0, h = 0, x0 = 0, y0 = 0;
  float advance = 0;
  std::vector<uint8_t> alpha;
};
std::map<std::tuple<bool, int, uint32_t>, Glyph> glyph_cache;

bool LoadFace(Face& face, const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  face.data.resize(size_t(n));
  const bool read = std::fread(face.data.data(), 1, face.data.size(), f) == face.data.size();
  std::fclose(f);
  face.ok = read && stbtt_InitFont(&face.info, face.data.data(), 0);
  return face.ok;
}

const Glyph& GlyphOf(bool bold, int size, uint32_t codepoint) {
  auto key = std::make_tuple(bold, size, codepoint);
  auto it = glyph_cache.find(key);
  if (it != glyph_cache.end()) return it->second;
  Glyph g;
  Face& face = faces[bold && faces[1].ok ? 1 : 0];
  if (face.ok) {
    const float scale = stbtt_ScaleForPixelHeight(&face.info, float(size));
    int advance = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&face.info, int(codepoint), &advance, &lsb);
    g.advance = advance * scale;
    int x1, y1;
    stbtt_GetCodepointBitmapBox(&face.info, int(codepoint), scale, scale, &g.x0, &g.y0, &x1, &y1);
    g.w = x1 - g.x0, g.h = y1 - g.y0;
    if (g.w > 0 && g.h > 0) {
      g.alpha.resize(size_t(g.w) * g.h);
      stbtt_MakeCodepointBitmap(&face.info, g.alpha.data(), g.w, g.h, g.w, scale, scale, int(codepoint));
    }
  }
  return glyph_cache.emplace(key, std::move(g)).first->second;
}

int Ascent(bool bold, int size) {
  Face& face = faces[bold && faces[1].ok ? 1 : 0];
  if (!face.ok) return size;
  int ascent, descent, gap;
  stbtt_GetFontVMetrics(&face.info, &ascent, &descent, &gap);
  return int(std::lround(ascent * stbtt_ScaleForPixelHeight(&face.info, float(size))));
}

// The next code point of UTF-8 text.
uint32_t NextCodepoint(const std::string& s, size_t& i) {
  const auto c = uint8_t(s[i++]);
  if (c < 0x80) return c;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
  uint32_t cp = c & (0x3F >> extra);
  while (extra-- > 0 && i < s.size()) cp = (cp << 6) | (uint8_t(s[i++]) & 0x3F);
  return cp;
}

inline void Blend(uint32_t& dst, Color color, uint32_t alpha) {
  const uint32_t a = ((color >> 24) * alpha) / 255;
  if (!a) return;
  const uint32_t ia = 255 - a;
  const uint32_t r = (((color >> 16) & 0xFF) * a + ((dst >> 16) & 0xFF) * ia) / 255;
  const uint32_t g = (((color >> 8) & 0xFF) * a + ((dst >> 8) & 0xFF) * ia) / 255;
  const uint32_t b = ((color & 0xFF) * a + (dst & 0xFF) * ia) / 255;
  dst = 0xFF000000u | (r << 16) | (g << 8) | b;
}

}  // namespace

bool Open(const char* font_regular, const char* font_bold) {
  LoadFace(faces[0], font_regular);
  LoadFace(faces[1], font_bold);
  MakeTiling();
  sceSystemServiceHideSplashScreen();
  video = sceVideoOutOpen(0xFF, 0, 0, nullptr);
  if (video < 0) return false;
  int64_t physical = 0;
  if (sceKernelAllocateDirectMemory(0, int64_t(sceKernelGetDirectMemorySize()), kBufferBytes * 2, kAlignment,
                                    kMemoryWriteCombined, &physical) < 0) {
    return false;
  }
  void* mapped = nullptr;
  if (sceKernelMapDirectMemory(&mapped, kBufferBytes * 2, kProtection, 0, physical, kAlignment) < 0) {
    return false;
  }
  buffers[0] = static_cast<uint8_t*>(mapped);
  buffers[1] = buffers[0] + kBufferBytes;
  std::memset(mapped, 0, kBufferBytes * 2);
  Flush(mapped, kBufferBytes * 2);
  struct {
    void* data;
    void* metadata;
    void* reserved[2];
  } registered[2] = {{buffers[0], nullptr, {}}, {buffers[1], nullptr, {}}};
  uint8_t attribute[80] = {};
  sceVideoOutSetFlipRate(video, 0);
  sceVideoOutSetBufferAttribute2(attribute, kPixelFormatBgra8, 0, kWidth, kHeight, 0, 0, 0);
  if (sceVideoOutRegisterBuffers2(video, 0, 0, registered, 2, attribute, 0, nullptr) < 0) return false;
  sceVideoOutSubmitFlip(video, 0, 1, ++flips);
  return true;
}

void Present() {
  if (video < 0) return;
  // (the buffer being shown isn't written: wait for the last flip first)
  while (sceVideoOutIsFlipPending(video) > 0) sceVideoOutWaitVblank(video);
  back ^= 1;
  uint8_t* out = buffers[back];
  for (int y = 0; y < kHeight; ++y) {
    const uint32_t* row = &picture[size_t(y) * kWidth];
    const uint32_t base = row_base[y], term = row_term[y];
    for (int x = 0; x < kWidth; ++x) {
      *reinterpret_cast<uint32_t*>(out + base + (column_offset[x] ^ term)) = row[x];
    }
  }
  const size_t used = size_t((kWidth + 127) / 128) * ((kHeight + 127) / 128) * 0x10000;
  Flush(out, used);
  sceVideoOutSubmitFlip(video, back, 1, ++flips);
  sceVideoOutWaitVblank(video);
}

void Clear(Color color) { std::fill(picture.begin(), picture.end(), color | 0xFF000000u); }

void FillRect(int x, int y, int w, int h, Color color) {
  const int x0 = std::max(0, x), y0 = std::max(0, y);
  const int x1 = std::min(kWidth, x + w), y1 = std::min(kHeight, y + h);
  const bool opaque = (color >> 24) == 0xFF;
  for (int py = y0; py < y1; ++py) {
    uint32_t* row = &picture[size_t(py) * kWidth];
    for (int px = x0; px < x1; ++px) {
      if (opaque) row[px] = color;
      else Blend(row[px], color, 255);
    }
  }
}

void Gradient(int x, int y, int w, int h, Color top, Color bottom) {
  for (int i = 0; i < h; ++i) {
    const float t = h > 1 ? float(i) / float(h - 1) : 0;
    auto mix = [t](uint32_t a, uint32_t b, int shift) {
      return uint32_t(std::lround(((a >> shift) & 0xFF) * (1 - t) + ((b >> shift) & 0xFF) * t)) << shift;
    };
    FillRect(x, y + i, w, 1, mix(top, bottom, 24) | mix(top, bottom, 16) | mix(top, bottom, 8) | mix(top, bottom, 0));
  }
}

void Frame(int x, int y, int w, int h, int t, Color color) {
  FillRect(x, y, w, t, color);
  FillRect(x, y + h - t, w, t, color);
  FillRect(x, y + t, t, h - 2 * t, color);
  FillRect(x + w - t, y + t, t, h - 2 * t, color);
}

int Text(int x, int y, const std::string& s, float size_f, Color color, bool bold) {
  const int size = int(std::lround(size_f));
  const int baseline = y + Ascent(bold, size);
  float pen = float(x);
  for (size_t i = 0; i < s.size();) {
    const Glyph& g = GlyphOf(bold, size, NextCodepoint(s, i));
    const int gx = int(std::lround(pen)) + g.x0, gy = baseline + g.y0;
    for (int row = 0; row < g.h; ++row) {
      const int py = gy + row;
      if (py < 0 || py >= kHeight) continue;
      for (int col = 0; col < g.w; ++col) {
        const int px = gx + col;
        if (px < 0 || px >= kWidth) continue;
        const uint8_t a = g.alpha[size_t(row) * g.w + col];
        if (a) Blend(picture[size_t(py) * kWidth + px], color, a);
      }
    }
    pen += g.advance;
  }
  return int(std::lround(pen)) - x;
}

int TextWidth(const std::string& s, float size_f, bool bold) {
  const int size = int(std::lround(size_f));
  float pen = 0;
  for (size_t i = 0; i < s.size();) pen += GlyphOf(bold, size, NextCodepoint(s, i)).advance;
  return int(std::lround(pen));
}

std::string Fit(const std::string& s, float size, int width, bool bold) {
  if (TextWidth(s, size, bold) <= width) return s;
  std::string t = s;
  while (!t.empty() && TextWidth(t + "...", size, bold) > width) {
    t.pop_back();
    while (!t.empty() && (uint8_t(t.back()) & 0xC0) == 0x80) t.pop_back();
    if (!t.empty() && uint8_t(t.back()) >= 0xC0) t.pop_back();
  }
  return t + "...";
}

namespace {

// Coverage (0..1) of a pixel by a rounded rectangle, from its distance to it.
inline float RoundCoverage(float px, float py, float x, float y, float w, float h, float r) {
  const float cx = std::clamp(px, x + r, x + w - r), cy = std::clamp(py, y + r, y + h - r);
  const float dx = px - cx, dy = py - cy;
  const float d = std::sqrt(dx * dx + dy * dy) - r;
  return std::clamp(0.5f - d, 0.0f, 1.0f);
}

inline void Put(int x, int y, Color color, float coverage) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight || coverage <= 0) return;
  Blend(picture[size_t(y) * kWidth + x], color, uint32_t(std::lround(std::min(coverage, 1.0f) * 255)));
}

Color Mix(Color a, Color b, float t) {
  auto c = [&](int s) {
    return uint32_t(std::lround(((a >> s) & 0xFF) * (1 - t) + ((b >> s) & 0xFF) * t)) << s;
  };
  return c(24) | c(16) | c(8) | c(0);
}

std::vector<uint32_t> saved;

}  // namespace

void Save() { saved = picture; }
const uint32_t* Pixels() { return picture.data(); }
void Restore() {
  if (saved.size() == picture.size()) picture = saved;
}

void RoundRectGradient(int x, int y, int w, int h, int radius, Color top, Color bottom) {
  const float r = float(std::min({radius, w / 2, h / 2}));
  for (int py = std::max(0, y); py < std::min(kHeight, y + h); ++py) {
    const Color color = Mix(top, bottom, h > 1 ? float(py - y) / float(h - 1) : 0);
    const bool middle = py >= y + r && py < y + h - r;
    for (int px = std::max(0, x); px < std::min(kWidth, x + w); ++px) {
      if (middle) {
        Blend(picture[size_t(py) * kWidth + px], color, 255);
      } else {
        Put(px, py, color, RoundCoverage(px + 0.5f, py + 0.5f, float(x), float(y), float(w), float(h), r));
      }
    }
  }
}

void RoundRect(int x, int y, int w, int h, int radius, Color color) { RoundRectGradient(x, y, w, h, radius, color, color); }

void RoundFrame(int x, int y, int w, int h, int radius, int t, Color color) {
  const float r = float(std::min({radius, w / 2, h / 2}));
  const float ri = std::max(0.0f, r - t);
  for (int py = std::max(0, y); py < std::min(kHeight, y + h); ++py) {
    for (int px = std::max(0, x); px < std::min(kWidth, x + w); ++px) {
      const float fx = px + 0.5f, fy = py + 0.5f;
      const float outer = RoundCoverage(fx, fy, float(x), float(y), float(w), float(h), r);
      const float inner = RoundCoverage(fx, fy, float(x + t), float(y + t), float(w - 2 * t), float(h - 2 * t), ri);
      Put(px, py, color, outer - inner);
    }
  }
}

void Circle(float cx, float cy, float r, Color color) {
  for (int py = int(cy - r - 1); py <= int(cy + r + 1); ++py) {
    for (int px = int(cx - r - 1); px <= int(cx + r + 1); ++px) {
      const float d = std::hypot(px + 0.5f - cx, py + 0.5f - cy);
      Put(px, py, color, r + 0.5f - d);
    }
  }
}

void Line(float x0, float y0, float x1, float y1, float t, Color color) {
  const float minx = std::min(x0, x1) - t, maxx = std::max(x0, x1) + t;
  const float miny = std::min(y0, y1) - t, maxy = std::max(y0, y1) + t;
  const float dx = x1 - x0, dy = y1 - y0, len2 = std::max(dx * dx + dy * dy, 1e-6f);
  for (int py = int(miny); py <= int(maxy); ++py) {
    for (int px = int(minx); px <= int(maxx); ++px) {
      const float fx = px + 0.5f, fy = py + 0.5f;
      const float u = std::clamp(((fx - x0) * dx + (fy - y0) * dy) / len2, 0.0f, 1.0f);
      const float d = std::hypot(fx - (x0 + u * dx), fy - (y0 + u * dy));
      Put(px, py, color, t / 2 + 0.5f - d);
    }
  }
}

void Shadow(int x, int y, int w, int h, int radius, int spread, Color color) {
  const float a = float(color >> 24) / 255.0f;
  for (int py = std::max(0, y - spread); py < std::min(kHeight, y + h + spread); ++py) {
    for (int px = std::max(0, x - spread); px < std::min(kWidth, x + w + spread); ++px) {
      const float r = float(radius);
      const float cx = std::clamp(px + 0.5f, x + r, x + w - r), cy = std::clamp(py + 0.5f, y + r, y + h - r);
      const float d = std::max(0.0f, std::hypot(px + 0.5f - cx, py + 0.5f - cy) - r);
      const float t = 1.0f - std::min(d / float(spread), 1.0f);
      Put(px, py, color | 0xFF000000u, a * t * t);
    }
  }
}

void Glow(float cx, float cy, float r, Color color) {
  const float a = float(color >> 24) / 255.0f;
  for (int py = std::max(0, int(cy - r)); py < std::min(kHeight, int(cy + r)); ++py) {
    for (int px = std::max(0, int(cx - r)); px < std::min(kWidth, int(cx + r)); ++px) {
      const float d = std::hypot(px - cx, py - cy) / r;
      if (d < 1) Put(px, py, color | 0xFF000000u, a * (1 - d) * (1 - d));
    }
  }
}

void Image(int x, int y, int w, int h, const std::vector<uint8_t>& rgba, int sw, int sh, int radius) {
  if (rgba.empty() || sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return;
  for (int py = std::max(0, y); py < std::min(kHeight, y + h); ++py) {
    const float fy = std::clamp((py - y + 0.5f) * sh / h - 0.5f, 0.0f, float(sh - 1));
    const int y0 = int(fy), y1 = std::min(y0 + 1, sh - 1);
    const float ty = fy - y0;
    for (int px = std::max(0, x); px < std::min(kWidth, x + w); ++px) {
      const float fx = std::clamp((px - x + 0.5f) * sw / w - 0.5f, 0.0f, float(sw - 1));
      const int x0 = int(fx), x1 = std::min(x0 + 1, sw - 1);
      const float tx = fx - x0;
      float c[4];
      for (int k = 0; k < 4; ++k) {
        auto at = [&](int ix, int iy) { return float(rgba[(size_t(iy) * sw + ix) * 4 + k]); };
        c[k] = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
      }
      float coverage = c[3] / 255.0f;
      if (radius > 0) {
        coverage *= RoundCoverage(px + 0.5f, py + 0.5f, float(x), float(y), float(w), float(h), float(radius));
      }
      const Color color = 0xFF000000u | uint32_t(c[0]) << 16 | uint32_t(c[1]) << 8 | uint32_t(c[2]);
      Put(px, py, color, coverage);
    }
  }
}

}  // namespace screen
