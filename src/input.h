// The DualSense of the user who opened the app (scePad).

#pragma once

#include <cstdint>

namespace input {

enum Button : uint32_t {
  kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80,
  kL1 = 0x400, kR1 = 0x800, kTriangle = 0x1000, kCircle = 0x2000,
  kCross = 0x4000, kSquare = 0x8000, kOptions = 0x8,
};

void Open();
// Buttons pressed since the last call (with key repeat for the d-pad).
uint32_t Pressed();

// The user the app runs for (its home folder: /user/home/%08x).
int32_t User();

}  // namespace input
