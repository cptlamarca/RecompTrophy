// An Xbox 360 game's achievements, read from its disc image: the image's
// default.xex (the GDFX file system: sector 32 of the game partition holds
// the root folder), its executable decrypted and decompressed (the retail or
// devkit key; raw, basic or LZX compression) and the XDBF in its resources
// (XACH achievements, XSTR strings per language, PNG pictures).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace game {

struct Achievement {
  uint32_t id = 0;
  std::string name[2], detail[2];  // English, Portuguese (English when none)
  uint32_t gamerscore = 0;
  bool hidden = false;             // (no "show unachieved" flag)
  std::vector<uint8_t> picture;    // PNG (64x64 on the Xbox)
};

struct Info {
  uint32_t title_id = 0;
  std::string title;
  std::vector<uint8_t> icon;  // PNG
  std::vector<Achievement> achievements;
};

// iso_path: a disc image, or an installed game's default.xex.
// False with `error` set (a message for the player) when it can't.
// progress: 0..1 as it goes (may be null).
bool Read(const std::string& iso_path, Info& info, std::string& error, float* progress);

}  // namespace game
