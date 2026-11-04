// The app's screen: a 1920x1080 picture drawn by the CPU (ordinary memory)
// and shown through VideoOut, copied into the display's tiled buffers when it
// changed. Text with Noto Sans (stb_truetype); pictures from RGBA pixels.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace screen {

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;

// 0xAARRGGBB
using Color = uint32_t;

// False when the display or its memory couldn't be had.
bool Open(const char* font_regular, const char* font_bold);

// Shows what was drawn since the last Present (waits for the next vblank).
void Present();

void Clear(Color color);
void FillRect(int x, int y, int w, int h, Color color);  // (alpha blended)
void Gradient(int x, int y, int w, int h, Color top, Color bottom);
void Frame(int x, int y, int w, int h, int thickness, Color color);

// Text at (x, y) as its top-left; returns its width. bold: the bold face.
int Text(int x, int y, const std::string& utf8, float size, Color color, bool bold = false);
int TextWidth(const std::string& utf8, float size, bool bold = false);
// Cut with "..." to fit `width`.
std::string Fit(const std::string& utf8, float size, int width, bool bold = false);

// RGBA pixels (src_w x src_h) scaled (bilinear) into the rectangle, its
// corners rounded by `radius`.
void Image(int x, int y, int w, int h, const std::vector<uint8_t>& rgba, int src_w, int src_h, int radius = 0);

// Shapes, anti-aliased, alpha blended.
void RoundRect(int x, int y, int w, int h, int radius, Color color);
void RoundRectGradient(int x, int y, int w, int h, int radius, Color top, Color bottom);
void RoundFrame(int x, int y, int w, int h, int radius, int thickness, Color color);
void Circle(float cx, float cy, float r, Color color);
void Line(float x0, float y0, float x1, float y1, float thickness, Color color);
// A soft shadow under a rounded rectangle.
void Shadow(int x, int y, int w, int h, int radius, int spread, Color color);
// A soft round glow.
void Glow(float cx, float cy, float r, Color color);

// The picture kept (a background drawn once) and put back.
void Save();
void Restore();

// The picture drawn (0xAARRGGBB, kWidth x kHeight): for previews off the console.
const uint32_t* Pixels();

}  // namespace screen
