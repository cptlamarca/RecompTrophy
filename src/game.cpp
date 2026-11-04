// See game.h.

#include "game.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

extern "C" {
#include "aes.h"
#include "mspack.h"
#include "lzx.h"
}

#include "text.h"

using text::T;

namespace game {
namespace {

using Bytes = std::vector<uint8_t>;

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }

// ---- the disc image ---------------------------------------------------------

constexpr uint64_t kSector = 2048;

bool ReadAt(FILE* f, uint64_t offset, void* out, size_t size) {
  return fseeko(f, off_t(offset), SEEK_SET) == 0 && fread(out, 1, size, f) == size;
}

// default.xex from the root folder of the image's game partition.
bool ReadDefaultXex(const std::string& path, Bytes& xex, std::string& error) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    error = T("The disc image can't be opened.", "Não foi possível abrir a imagem do disco.");
    return false;
  }
  static const uint64_t kPartitions[] = {0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000};
  uint64_t game = 0;
  uint8_t head[32];
  bool found = false;
  for (uint64_t at : kPartitions) {
    if (ReadAt(f, at + 32 * kSector, head, sizeof(head)) && std::memcmp(head, "MICROSOFT*XBOX*MEDIA", 20) == 0) {
      game = at, found = true;
      break;
    }
  }
  if (!found) {
    std::fclose(f);
    error = T("This isn't an Xbox 360 disc image.", "Isto não é uma imagem de disco de Xbox 360.");
    return false;
  }
  const uint32_t root_sector = Le32(head + 20), root_size = Le32(head + 24);
  Bytes root(root_size);
  if (root_size < 14 || root_size > (32u << 20) || !ReadAt(f, game + root_sector * kSector, root.data(), root.size())) {
    std::fclose(f);
    error = T("The disc image is damaged.", "A imagem do disco está danificada.");
    return false;
  }
  // the root folder: a binary tree of entries (left, right in 4 byte units,
  // sector, size, attributes, name)
  std::vector<uint16_t> pending = {0};
  uint32_t sector = 0, size = 0;
  bool have = false;
  for (int guard = 0; !pending.empty() && guard < 65536 && !have; ++guard) {
    const size_t at = size_t(pending.back()) * 4;
    pending.pop_back();
    if (at + 14 > root.size()) continue;
    const uint8_t* e = &root[at];
    const uint8_t name_length = e[13];
    if (at + 14 + name_length > root.size()) continue;
    if (name_length == 11 && strncasecmp(reinterpret_cast<const char*>(e + 14), "default.xex", 11) == 0) {
      sector = Le32(e + 4), size = Le32(e + 8), have = true;
    }
    if (Le16(e)) pending.push_back(Le16(e));
    if (Le16(e + 2)) pending.push_back(Le16(e + 2));
  }
  if (!have || size < 0x200 || size > (512u << 20)) {
    std::fclose(f);
    error = T("The disc has no default.xex.", "O disco não tem o default.xex.");
    return false;
  }
  xex.resize(size);
  const bool ok = ReadAt(f, game + uint64_t(sector) * kSector, xex.data(), xex.size());
  std::fclose(f);
  if (!ok) error = T("The disc image can't be read.", "Não foi possível ler a imagem do disco.");
  return ok;
}

// ---- LZX (libmspack, from memory) ---------------------------------------------

struct MemFile {
  const uint8_t* in = nullptr;
  size_t in_size = 0, at = 0;
  Bytes* out = nullptr;
};

struct mspack_file* MemOpen(struct mspack_system*, const char*, int) { return nullptr; }
void MemClose(struct mspack_file*) {}
int MemRead(struct mspack_file* file, void* buffer, int bytes) {
  auto* f = reinterpret_cast<MemFile*>(file);
  const size_t n = std::min(size_t(bytes), f->in_size - f->at);
  std::memcpy(buffer, f->in + f->at, n);
  f->at += n;
  return int(n);
}
int MemWrite(struct mspack_file* file, void* buffer, int bytes) {
  auto* f = reinterpret_cast<MemFile*>(file);
  f->out->insert(f->out->end(), static_cast<uint8_t*>(buffer), static_cast<uint8_t*>(buffer) + bytes);
  return bytes;
}
int MemSeek(struct mspack_file*, off_t, int) { return 1; }
off_t MemTell(struct mspack_file* file) { return off_t(reinterpret_cast<MemFile*>(file)->at); }
void MemMessage(struct mspack_file*, const char*, ...) {}
void* MemAlloc(struct mspack_system*, size_t bytes) { return std::calloc(bytes, 1); }
void MemFree(void* p) { std::free(p); }
void MemCopy(void* src, void* dst, size_t bytes) { std::memmove(dst, src, bytes); }

bool Unlzx(const Bytes& stream, uint32_t window, uint32_t image_size, Bytes& image) {
  struct mspack_system sys = {MemOpen, MemClose, MemRead, MemWrite, MemSeek, MemTell,
                              MemMessage, MemAlloc, MemFree, MemCopy, nullptr};
  int bits = 0;
  while ((1u << bits) < window) ++bits;
  MemFile in, out;
  in.in = stream.data(), in.in_size = stream.size();
  image.clear();
  image.reserve(image_size);
  out.out = &image;
  struct lzxd_stream* z = lzxd_init(&sys, reinterpret_cast<mspack_file*>(&in), reinterpret_cast<mspack_file*>(&out),
                                    bits, 0, 0x8000, off_t(image_size), 0);
  if (!z) return false;
  const int rc = lzxd_decompress(z, off_t(image_size));
  lzxd_free(z);
  return rc == MSPACK_ERR_OK && image.size() == image_size;
}

// ---- the executable ---------------------------------------------------------------

bool XexImage(const Bytes& x, Bytes& image, uint32_t& title_id, std::string& error) {
  error = T("The game's executable can't be read.", "Não foi possível ler o executável do jogo.");
  if (x.size() < 24 || std::memcmp(x.data(), "XEX2", 4) != 0) return false;
  const uint32_t pe_offset = Be32(&x[8]), security = Be32(&x[16]), count = Be32(&x[20]);
  uint32_t format = 0, execution = 0;
  for (uint32_t i = 0; i < count && 24 + i * 8 + 8 <= x.size(); ++i) {
    const uint32_t key = Be32(&x[24 + i * 8]), value = Be32(&x[28 + i * 8]);
    if (key == 0x3FF) format = value;
    if (key == 0x40006) execution = value;
  }
  if (!format || format + 8 > x.size() || security + 0x160 > x.size() || pe_offset > x.size()) return false;
  if (execution && execution + 16 <= x.size()) title_id = Be32(&x[execution + 12]);
  const uint32_t format_size = Be32(&x[format]);
  const uint16_t encryption = Be16(&x[format + 4]), compression = Be16(&x[format + 6]);
  const uint32_t image_size = Be32(&x[security + 4]);
  if (image_size == 0 || image_size > (512u << 20)) return false;

  const Bytes raw(x.begin() + pe_offset, x.end());
  static const uint8_t kKeys[2][16] = {
      {0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3, 0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91},  // retail
      {}};                                                                                                // devkit
  for (int k = 0; k < (encryption ? 2 : 1); ++k) {
    Bytes data = raw;
    if (encryption) {
      uint8_t session[16];
      std::memcpy(session, &x[security + 0x150], 16);
      AES_ctx ctx;
      AES_init_ctx(&ctx, kKeys[k]);
      AES_ECB_decrypt(&ctx, session);
      uint8_t iv[16] = {};
      AES_init_ctx_iv(&ctx, session, iv);
      AES_CBC_decrypt_buffer(&ctx, data.data(), uint32_t(data.size() & ~size_t(15)));
    }
    image.clear();
    if (compression == 0) {
      image = data;
      image.resize(image_size);
    } else if (compression == 1) {
      // blocks of (data size, zeros) after the header
      size_t p = 0;
      for (uint32_t b = 0; b < (format_size - 8) / 8; ++b) {
        const uint32_t data_size = Be32(&x[format + 8 + b * 8]), zeros = Be32(&x[format + 12 + b * 8]);
        if (p + data_size > data.size()) break;
        image.insert(image.end(), data.begin() + p, data.begin() + p + data_size);
        image.insert(image.end(), zeros, 0);
        p += data_size;
      }
    } else if (compression == 2) {
      // blocks (the next one's size and hash, then chunks of a 16 bit size
      // and LZX data, ending with 0)
      const uint32_t window = Be32(&x[format + 8]);
      uint32_t block_size = Be32(&x[format + 12]);
      Bytes stream;
      size_t p = 0;
      while (block_size && p + block_size <= data.size()) {
        const uint8_t* block = &data[p];
        const uint32_t next = Be32(block);
        size_t q = 24;
        while (q + 2 <= block_size) {
          const uint32_t chunk = Be16(block + q);
          q += 2;
          if (!chunk || q + chunk > block_size) break;
          stream.insert(stream.end(), block + q, block + q + chunk);
          q += chunk;
        }
        p += block_size;
        block_size = next;
      }
      if (!Unlzx(stream, window, image_size, image)) image.clear();
    } else {
      error = T("This game's executable is in a format this app doesn't read.",
                "O executável deste jogo está num formato que este app não lê.");
      return false;
    }
    if (image.size() >= 2 && image[0] == 'M' && image[1] == 'Z') return true;
  }
  return false;
}

// ---- the XDBF ------------------------------------------------------------------------

struct Xdbf {
  const uint8_t* base = nullptr;
  size_t size = 0;
  struct Entry {
    uint16_t section;
    uint64_t id;
    uint32_t offset, size;
  };
  std::vector<Entry> entries;
  size_t data = 0;

  bool Parse(const uint8_t* p, size_t n) {
    if (n < 24 || std::memcmp(p, "XDBF", 4) != 0) return false;
    const uint32_t table = Be32(p + 8), used = Be32(p + 12), free_table = Be32(p + 16);
    data = 24 + size_t(table) * 18 + size_t(free_table) * 8;
    if (used > table || data > n) return false;
    base = p, size = n;
    for (uint32_t i = 0; i < used; ++i) {
      const uint8_t* e = p + 24 + i * 18;
      entries.push_back({Be16(e), Be64(e + 2), Be32(e + 10), Be32(e + 14)});
    }
    return true;
  }
  const uint8_t* Get(uint16_t section, uint64_t id, uint32_t* out_size) const {
    for (const auto& e : entries) {
      if (e.section == section && e.id == id && data + e.offset + e.size <= size) {
        *out_size = e.size;
        return base + data + e.offset;
      }
    }
    return nullptr;
  }
  static uint64_t Tag(const char* s) {
    uint64_t v = 0;
    for (int i = 0; s[i]; ++i) v = v << 8 | uint8_t(s[i]);
    return v;
  }
  // The strings of a language (XSTR, section 3, id the language).
  bool Strings(uint64_t language, std::vector<std::pair<uint16_t, std::string>>& out) const {
    uint32_t n = 0;
    const uint8_t* x = Get(3, language, &n);
    if (!x || n < 14 || std::memcmp(x, "XSTR", 4) != 0) return false;
    const uint16_t count = Be16(x + 12);
    size_t p = 14;
    for (uint16_t i = 0; i < count && p + 4 <= n; ++i) {
      const uint16_t id = Be16(x + p), length = Be16(x + p + 2);
      if (p + 4 + length > n) break;
      out.emplace_back(id, std::string(reinterpret_cast<const char*>(x + p + 4), length));
      p += 4 + length;
    }
    return true;
  }
};

std::string Find(const std::vector<std::pair<uint16_t, std::string>>& strings, uint16_t id) {
  for (const auto& [i, s] : strings) {
    if (i == id) return s;
  }
  return {};
}

}  // namespace

bool Read(const std::string& iso_path, Info& info, std::string& error, float* progress) {
  auto step = [&](float v) {
    if (progress) *progress = v;
  };
  step(0.05f);
  Bytes xex, image;
  // (a disc image, or an installed game's default.xex)
  const bool is_xex = iso_path.size() > 4 && strcasecmp(iso_path.c_str() + iso_path.size() - 4, ".xex") == 0;
  if (is_xex) {
    FILE* f = std::fopen(iso_path.c_str(), "rb");
    if (f) {
      fseeko(f, 0, SEEK_END);
      const off_t size = ftello(f);
      fseeko(f, 0, SEEK_SET);
      if (size > 0 && size < (512 << 20)) {
        xex.resize(size_t(size));
        if (std::fread(xex.data(), 1, xex.size(), f) != xex.size()) xex.clear();
      }
      std::fclose(f);
    }
    if (xex.empty()) {
      error = T("The game's executable can't be read.", "Não foi possível ler o executável do jogo.");
      return false;
    }
  } else if (!ReadDefaultXex(iso_path, xex, error)) {
    return false;
  }
  step(0.3f);
  if (!XexImage(xex, image, info.title_id, error)) return false;
  step(0.8f);
  // the XDBF: the one of the image's with achievements
  Xdbf db;
  bool found = false;
  for (size_t at = 0; !found && at + 4 <= image.size(); ++at) {
    if (image[at] != 'X' || std::memcmp(&image[at], "XDBF", 4) != 0) continue;
    Xdbf candidate;
    uint32_t n = 0;
    if (candidate.Parse(&image[at], image.size() - at) && candidate.Get(1, Xdbf::Tag("XACH"), &n)) {
      db = candidate;
      found = true;
    }
  }
  if (!found) {
    error = T("This game has no achievements.", "Este jogo não tem conquistas.");
    return false;
  }
  std::vector<std::pair<uint16_t, std::string>> english, portuguese;
  db.Strings(1, english);  // (XLanguage 1 English, 9 Portuguese)
  if (!db.Strings(9, portuguese)) portuguese = english;
  if (english.empty()) english = portuguese;
  // the title: XTHD's title string (id 0x8000 in the default language)
  info.title = Find(english, 0x8000);
  uint32_t n = 0;
  if (const uint8_t* icon = db.Get(2, 0x8000, &n)) info.icon.assign(icon, icon + n);
  const uint8_t* x = db.Get(1, Xdbf::Tag("XACH"), &n);
  if (!x || n < 14) {
    error = T("This game has no achievements.", "Este jogo não tem conquistas.");
    return false;
  }
  const uint16_t count = Be16(x + 12);
  for (uint16_t i = 0; i < count && 14 + (i + 1) * 36 <= n; ++i) {
    const uint8_t* e = x + 14 + i * 36;
    Achievement a;
    a.id = Be16(e);
    const uint16_t label = Be16(e + 2), detail = Be16(e + 4);
    a.name[0] = Find(english, label), a.detail[0] = Find(english, detail);
    a.name[1] = Find(portuguese, label), a.detail[1] = Find(portuguese, detail);
    if (a.name[1].empty()) a.name[1] = a.name[0], a.detail[1] = a.detail[0];
    a.gamerscore = Be16(e + 12);
    a.hidden = !(Be32(e + 16) & 8);
    uint32_t picture_size = 0;
    if (const uint8_t* picture = db.Get(2, Be32(e + 8), &picture_size)) a.picture.assign(picture, picture + picture_size);
    if (a.id >= 1 && a.id <= 127) info.achievements.push_back(std::move(a));
  }
  if (info.achievements.empty()) {
    error = T("This game has no achievements.", "Este jogo não tem conquistas.");
    return false;
  }
  if (info.title.empty()) info.title = "Xbox 360";
  error.clear();
  step(1.0f);
  return true;
}

}  // namespace game
