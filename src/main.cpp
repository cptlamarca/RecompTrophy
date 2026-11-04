// RecompTrophy: the PS5 trophies of an Xbox 360 game, made from its disc
// image. The player picks an image (looked for on the console and its drives;
// each shown with its game's name, icon and achievements, read in the
// background); the app reads the game's achievements (src/game.h), makes its
// trophy set (src/trophyset.h) and has the relay install it and the player's
// progress, with the achievements already earned in the game's port or an
// emulator's profile (src/install.h). The game then shows in the console's
// trophy list; a port with trophy support unlocks them as they're earned.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "game.h"
#include "input.h"
#include "install.h"
#include "screen.h"
#include "text.h"
#include "trophyset.h"
#include "third_party/qrcodegen/qrcodegen.hpp"

extern "C" {
unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y, int* comp, int req);
void stbi_image_free(void* data);
}

using text::T;

namespace {

// ---- pictures ------------------------------------------------------------------------

struct Rgba {
  std::vector<uint8_t> pixels;
  int w = 0, h = 0;
  bool empty() const { return pixels.empty(); }
};

Rgba Decode(const std::vector<uint8_t>& png) {
  Rgba out;
  int n = 0;
  if (png.empty()) return out;
  if (unsigned char* p = stbi_load_from_memory(png.data(), int(png.size()), &out.w, &out.h, &n, 4)) {
    out.pixels.assign(p, p + size_t(out.w) * out.h * 4);
    stbi_image_free(p);
  }
  return out;
}

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// ---- what's been made (/data/trophies360/done.txt: the sets' ids) ---------------------

constexpr char kDoneFile[] = "/data/trophies360/done.txt";

std::set<std::string> Done() {
  std::set<std::string> done;
  std::ifstream in(kDoneFile);
  std::string id;
  while (in >> id) done.insert(id);
  return done;
}

void MarkDone(const std::string& set_id) {
  if (Done().count(set_id)) return;
  std::ofstream(kDoneFile, std::ios::app) << set_id << '\n';
}

// ---- the native notifications of a game ---------------------------------------------
//
// The watcher (payload/watcher.c) shows them for the games made, unless turned
// off here (watch/SET/off) or the game's port shows its own (native/SET, left
// by the port: then it would show them twice).

enum class Notifications { kNone, kOn, kOff, kNative };

bool FileExists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

std::string NativeMarker(const std::string& set_id) { return "/data/trophies360/native/" + set_id; }

Notifications NotificationsOf(const std::string& set_id) {
  if (set_id.empty()) return Notifications::kNone;
  // (Army of Two: The Devil's Cartel's port shows its own, from before the marker)
  if (set_id == "NPWR99990_00" || FileExists(NativeMarker(set_id))) return Notifications::kNative;
  const std::string folder = std::string(install::kWatchFolder) + "/" + set_id;
  if (!FileExists(folder + "/game.txt")) return Notifications::kNone;
  return FileExists(folder + "/off") ? Notifications::kOff : Notifications::kOn;
}

void SetNotifications(const std::string& set_id, bool on) {
  const std::string off = std::string(install::kWatchFolder) + "/" + set_id + "/off";
  if (on) {
    std::remove(off.c_str());
  } else {
    std::ofstream(off, std::ios::trunc) << "off\n";
  }
}

// ---- the disc images ----------------------------------------------------------------

struct Entry {
  std::string path;
  uint64_t size = 0;
  bool installed = false;  // an installed game's default.xex (not a disc image)
  bool emulator = false;   // a game of an Xbox 360 emulator's library (PS5X360's)
  // read in the background
  bool read = false, ok = false;
  std::string name, set_id;
  size_t achievements = 0;
  Rgba icon;
  Notifications notifications = Notifications::kNone;
};

std::vector<Entry> Search() {
  std::vector<Entry> images;
  std::vector<std::filesystem::path> roots = {"/data", "/user/download", "/mnt/ext0", "/mnt/ext1"};
  for (int i = 0; i < 8; ++i) roots.push_back("/mnt/usb" + std::to_string(i));
  const std::vector<std::string> skip = {"assets",  "radv-shader-cache", "textures", "UserData", "logs",
                                         "sce_sys", "sce_module",        "cache",    "shaders",  "Data",
                                         "Movies",  "Content",           "$SystemUpdate", "NxeArt"};
  const auto started = std::chrono::steady_clock::now();
  size_t visited = 0;
  std::error_code ec;
  for (const auto& root : roots) {
    if (!std::filesystem::is_directory(root, ec)) continue;
    std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied,
                                                     ec);
    for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (!(++visited & 255) && std::chrono::steady_clock::now() - started > std::chrono::seconds(6)) break;
      std::error_code e;
      if (it->is_symlink(e)) {
        it.disable_recursion_pending();
        continue;
      }
      const std::string name = it->path().filename().string();
      if (it->is_directory(e)) {
        if (it.depth() >= 5 || name.starts_with(".") || std::find(skip.begin(), skip.end(), name) != skip.end()) {
          it.disable_recursion_pending();
        }
        continue;
      }
      std::string extension = it->path().extension().string();
      std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
      if (extension != ".iso" || !it->is_regular_file(e)) continue;
      // (the same image twice, as a store's copy: once)
      const uint64_t size = it->file_size(e);
      if (std::none_of(images.begin(), images.end(), [&](const Entry& i) {
            return i.size == size && std::filesystem::path(i.path).filename() == it->path().filename();
          })) {
        Entry entry;
        entry.path = it->path().string();
        entry.size = size;
        images.push_back(std::move(entry));
      }
    }
    ec.clear();
  }
  std::sort(images.begin(), images.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
  // the games installed in ports (their disc's files extracted: default.xex
  // in assets/, as the ports', or deeper, as LibertyRecomp's), before the images
  std::vector<Entry> installed;
  std::filesystem::directory_iterator apps("/data/homebrew", ec);
  for (; !ec && apps != std::filesystem::directory_iterator(); apps.increment(ec)) {
    std::error_code e;
    if (!apps->is_directory(e)) continue;
    const std::filesystem::path app = apps->path();
    std::vector<std::filesystem::path> candidates = {app / "assets" / "default.xex", app / "assets" / "Default.xex"};
    std::filesystem::directory_iterator user(app / "UserData", e);
    for (; !e && user != std::filesystem::directory_iterator(); user.increment(e)) {
      candidates.push_back(user->path() / "game" / "default.xex");
      candidates.push_back(user->path() / "assets" / "default.xex");
    }
    for (const auto& c : candidates) {
      std::error_code x;
      if (std::filesystem::is_regular_file(c, x)) {
        Entry entry;
        entry.path = c.string();
        entry.installed = true;
        installed.push_back(std::move(entry));
        break;
      }
    }
  }
  ec.clear();
  std::sort(installed.begin(), installed.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
  // the Xbox 360 emulators' libraries (PS5X360's: a folder per game - its
  // default.xex extracted, or an .iso - in its assets/roms, /data/xbox360 and
  // the drives' xbox360; GOD / STFS packages aren't read)
  std::vector<Entry> emulated;
  auto add_emulated = [&](const std::filesystem::path& path) {
    for (const Entry& e : emulated) {
      if (e.path == path.string()) return;
    }
    Entry entry;
    entry.path = path.string();
    entry.emulator = true;
    std::error_code e;
    entry.size = std::filesystem::is_regular_file(path, e) && path.extension() != ".xex" &&
                         path.extension() != ".XEX"
                     ? std::filesystem::file_size(path, e)
                     : 0;
    emulated.push_back(std::move(entry));
  };
  std::vector<std::filesystem::path> libraries = {"/data/xbox360", "/mnt/ext0/xbox360", "/mnt/ext1/xbox360"};
  for (int i = 0; i < 8; ++i) libraries.push_back("/mnt/usb" + std::to_string(i) + "/xbox360");
  std::filesystem::directory_iterator emulators("/data/homebrew", ec);
  for (; !ec && emulators != std::filesystem::directory_iterator(); emulators.increment(ec)) {
    std::error_code e;
    if (std::filesystem::is_directory(emulators->path() / "assets" / "roms", e)) {
      libraries.push_back(emulators->path() / "assets" / "roms");
    }
  }
  ec.clear();
  for (const auto& library : libraries) {
    std::error_code e;
    std::filesystem::directory_iterator games(library, e);
    for (; !e && games != std::filesystem::directory_iterator(); games.increment(e)) {
      const std::filesystem::path game = games->path();
      std::error_code x;
      auto is_iso = [](const std::filesystem::path& p) {
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return ext == ".iso";
      };
      if (std::filesystem::is_regular_file(game, x)) {
        if (is_iso(game)) add_emulated(game);
        continue;
      }
      if (!std::filesystem::is_directory(game, x)) continue;
      bool found = false;
      for (const char* name : {"default.xex", "Default.xex", "DEFAULT.XEX"}) {
        if (!found && std::filesystem::is_regular_file(game / name, x)) add_emulated(game / name), found = true;
      }
      std::filesystem::directory_iterator inside(game, x);
      for (; !found && !x && inside != std::filesystem::directory_iterator(); inside.increment(x)) {
        if (is_iso(inside->path())) add_emulated(inside->path()), found = true;
      }
    }
  }
  std::sort(emulated.begin(), emulated.end(), [](const Entry& a, const Entry& b) { return a.path < b.path; });
  // (the installed games first, then the emulator's, then the disc images;
  // an image in an emulator's library shown once, as the emulator's)
  images.erase(std::remove_if(images.begin(), images.end(),
                              [&](const Entry& i) {
                                return std::any_of(emulated.begin(), emulated.end(),
                                                   [&](const Entry& e) { return e.path == i.path; });
                              }),
               images.end());
  installed.insert(installed.end(), emulated.begin(), emulated.end());
  installed.insert(installed.end(), images.begin(), images.end());
  return installed;
}

// "PPSA95290" of /data/homebrew/PPSA95290/...
std::string AppOf(const std::string& path) {
  const std::string prefix = "/data/homebrew/";
  if (path.rfind(prefix, 0) != 0) return {};
  return path.substr(prefix.size(), path.find('/', prefix.size()) - prefix.size());
}

std::string Size(uint64_t bytes) {
  char out[32];
  std::snprintf(out, sizeof(out), "%.1f GB", double(bytes) / (1024.0 * 1024.0 * 1024.0));
  return out;
}

// ---- the work ------------------------------------------------------------------------

enum class State { kSearching, kList, kWorking, kDone, kError };

struct Work {
  std::mutex mutex;
  std::atomic<State> state{State::kSearching};
  std::atomic<int> changes{0};  // (something to draw again)
  std::vector<Entry> images;
  std::set<std::string> done;
  // working
  std::atomic<int> step{0};  // 0 reading the disc, 1 making the trophies, 2 installing
  std::atomic<float> progress{0};
  std::string working_name;
  Rgba working_icon;
  // done / error
  std::string title, error, port_name;
  size_t trophies = 0, imported = 0;
  int grades[5] = {};  // by grade: 1 platinum .. 4 bronze
  bool updated = false, watching = false, native = false;
  Rgba icon;
} work;

std::atomic<int> search_generation{0};

// Each image's game, in the background (name, icon, achievements).
void ReadInfo(int generation) {
  std::thread([generation] {
    for (size_t i = 0;; ++i) {
      std::string path;
      {
        std::lock_guard<std::mutex> lock(work.mutex);
        if (generation != search_generation || i >= work.images.size()) return;
        path = work.images[i].path;
      }
      game::Info info;
      std::string error;
      const bool ok = game::Read(path, info, error, nullptr);
      install::Port port;
      const bool has_port = ok && install::FindPort(info.title_id, port);
      Rgba icon = Decode(has_port && !port.icon.empty() && !port.name.empty() ? port.icon : info.icon);
      if (ok && path.find("/assets/roms/") != std::string::npos) {
        char tile[32];
        std::snprintf(tile, sizeof(tile), "%08X-tile.png", info.title_id);
        const std::string app = path.substr(0, path.find("/assets/roms/"));
        Rgba art = Decode(ReadFile(app + "/game-art/" + tile));
        if (!art.empty()) icon = std::move(art);
      }
      std::lock_guard<std::mutex> lock(work.mutex);
      if (generation != search_generation || i >= work.images.size()) return;
      Entry& e = work.images[i];
      e.read = true, e.ok = ok;
      e.name = has_port && !port.name.empty() ? port.name : info.title;
      e.achievements = info.achievements.size();
      e.set_id = ok ? trophyset::SetId(info.title_id) : "";
      e.notifications = NotificationsOf(e.set_id);
      e.icon = std::move(icon);
      ++work.changes;
    }
  }).detach();
}

void StartSearch() {
  work.state = State::kSearching;
  const int generation = ++search_generation;
  std::thread([generation] {
    auto images = Search();
    {
      std::lock_guard<std::mutex> lock(work.mutex);
      work.images = std::move(images);
      work.done = Done();
      work.state = State::kList;
      ++work.changes;
    }
    ReadInfo(generation);
  }).detach();
}

void Make(const Entry& image) {
  {
    std::lock_guard<std::mutex> lock(work.mutex);
    work.working_name = image.read && image.ok ? image.name
                        : image.installed ? AppOf(image.path)
                                          : std::filesystem::path(image.path).filename().string();
    work.working_icon = image.icon;
  }
  work.step = 0;
  work.progress = 0;
  work.state = State::kWorking;
  const std::string path = image.path;
  std::thread([path] {
    game::Info info;
    std::string error;
    float progress = 0;
    std::atomic<bool> busy{true};
    std::thread meter([&] {
      while (busy) {
        work.progress = progress;
        ++work.changes;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    });
    auto fail = [&](const std::string& message) {
      busy = false;
      meter.join();
      std::lock_guard<std::mutex> lock(work.mutex);
      work.error = message;
      work.state = State::kError;
      ++work.changes;
    };
    if (!game::Read(path, info, error, &progress)) return fail(error);
    install::Port port;
    const bool has_port = install::FindPort(info.title_id, port);
    const std::string name = has_port && !port.name.empty() ? port.name : info.title;
    work.step = 1;
    progress = 0;
    std::error_code ec;
    std::filesystem::create_directories(install::kJobFolder, ec);
    const std::string set_id = trophyset::SetId(info.title_id);
    if (!trophyset::Build(info, set_id, name, has_port ? port.icon : std::vector<uint8_t>{},
                          std::string(install::kJobFolder) + "/TROPHY.UCP",
                          std::string(install::kJobFolder) + "/trophies.txt", &progress)) {
      return fail(T("The trophies couldn't be written to /data.", "Não foi possível gravar os troféus em /data."));
    }
    work.step = 2;
    progress = 0.5f;
    // (the achievements earned, as the set's trophies)
    std::vector<std::pair<uint32_t, int64_t>> earned;
    for (const auto& [achievement, when] : port.earned) {
      if (const uint32_t trophy = trophyset::TrophyId(info, achievement)) earned.emplace_back(trophy, when);
    }
    const install::Result result = install::Run(set_id, input::User(), earned, info.achievements.size());
    busy = false;
    meter.join();
    if (!result.ok) return fail(result.error);
    MarkDone(set_id);
    // the native notifications: the watcher looks at its achievement files
    // from now on (not Army of Two: The Devil's Cartel's, whose port shows
    // its own)
    const bool native = set_id == "NPWR99990_00" || FileExists(NativeMarker(set_id));
    work.native = native;
    if (!native && !port.sources.empty()) {
      std::vector<std::pair<uint32_t, uint32_t>> trophies;
      std::vector<int> grades;
      std::vector<std::string> names;
      for (const auto& a : info.achievements) {
        trophies.emplace_back(a.id, trophyset::TrophyId(info, a.id));
        grades.push_back(a.gamerscore <= 10 ? 4 : a.gamerscore <= 25 ? 3 : 2);
        names.push_back(a.name[0]);
      }
      install::Watch(set_id, input::User(), name, trophies, grades, names, port.sources);
      work.watching = install::StartWatcher();
    } else {
      work.watching = false;
    }
    std::lock_guard<std::mutex> lock(work.mutex);
    work.done.insert(set_id);
    for (Entry& e : work.images) {
      if (e.set_id == set_id) e.notifications = NotificationsOf(set_id);
    }
    work.title = name;
    work.port_name = has_port ? port.name : "";
    work.trophies = info.achievements.size() + 1;
    work.imported = earned.size();
    work.updated = result.progress_state == "updated" || result.progress_state == "unchanged";
    for (int& g : work.grades) g = 0;
    work.grades[1] = 1;
    for (const auto& a : info.achievements) ++work.grades[a.gamerscore <= 10 ? 4 : a.gamerscore <= 25 ? 3 : 2];
    work.icon = Decode(has_port && !port.icon.empty() ? port.icon : info.icon);
    work.state = State::kDone;
    ++work.changes;
  }).detach();
}

// ---- drawing -----------------------------------------------------------------------------

constexpr screen::Color kText = 0xFFF4F6FA, kDim = 0xFF96A3B8, kFaint = 0xFF5C6880, kAccent = 0xFF3C82F6,
                        kAccentLight = 0xFF79A8FF, kGold = 0xFFF2C94C, kGreen = 0xFF4CC38A;
constexpr screen::Color kGradeColors[5] = {0, 0xFFC8DCF0, 0xFFF2C94C, 0xFFC9D1DA, 0xFFD39058};

Rgba app_icon;

void MakeBackground() {
  using namespace screen;
  Gradient(0, 0, kWidth, kHeight, 0xFF0E1A33, 0xFF04060B);
  Glow(260, 120, 900, 0x5C2F6BFF);
  Glow(1750, 1020, 760, 0x302F6BFF);
  Glow(1650, 120, 520, 0x18F2C94C);
  // fine diagonal lines, as the console's own backgrounds
  for (int i = -kHeight; i < kWidth; i += 46) Line(float(i), float(kHeight), float(i + kHeight), 0, 1.0f, 0x08FFFFFF);
  // the header
  if (!app_icon.empty()) {
    Shadow(100, 54, 92, 92, 20, 18, 0x80000000);
    Image(100, 54, 92, 92, app_icon.pixels, app_icon.w, app_icon.h, 20);
  }
  Text(220, 58, "RecompTrophy", 52, kText, true);
  Text(222, 122, T("PS5 trophies for your Xbox 360 games", "Troféus de PS5 para os seus jogos de Xbox 360"), 24, kDim);
  Save();
}

// The controller's button symbols, as the console draws them.
enum class Glyph { kCross, kCircle, kTriangle, kSquare };

int ButtonHint(int x, int y, Glyph glyph, const std::string& label) {
  using namespace screen;
  const float cx = float(x + 20), cy = float(y + 20);
  Circle(cx, cy, 19, 0xFF1C2638);
  switch (glyph) {
    case Glyph::kCross:
      Line(cx - 8, cy - 8, cx + 8, cy + 8, 3.2f, 0xFF7CB2FF);
      Line(cx - 8, cy + 8, cx + 8, cy - 8, 3.2f, 0xFF7CB2FF);
      break;
    case Glyph::kCircle:
      Circle(cx, cy, 9.5f, 0xFFFF6B6B);
      Circle(cx, cy, 6.5f, 0xFF1C2638);
      break;
    case Glyph::kTriangle:
      Line(cx, cy - 9, cx - 9, cy + 7, 3, 0xFF4CD6A0);
      Line(cx - 9, cy + 7, cx + 9, cy + 7, 3, 0xFF4CD6A0);
      Line(cx + 9, cy + 7, cx, cy - 9, 3, 0xFF4CD6A0);
      break;
    case Glyph::kSquare:
      RoundFrame(int(cx - 9), int(cy - 9), 18, 18, 2, 3, 0xFFF08CD0);
      break;
  }
  return x + 52 + Text(x + 52, y + 4, label, 28, kText) + 56;
}

void Hints(std::initializer_list<std::pair<Glyph, std::string>> hints) {
  screen::FillRect(0, screen::kHeight - 104, screen::kWidth, 1, 0x20FFFFFF);
  int x = 110;
  for (const auto& [glyph, label] : hints) x = ButtonHint(x, screen::kHeight - 76, glyph, label);
}

// A trophy cup of a grade, centered at (cx, cy), `s` tall.
void Cup(float cx, float cy, float s, screen::Color color) {
  using namespace screen;
  const float top = cy - s / 2;
  const float bowl_w = s * 0.62f, bowl_h = s * 0.42f;
  // handles
  for (int side : {-1, 1}) {
    const float hx = cx + side * bowl_w * 0.5f;
    Circle(hx, top + bowl_h * 0.35f, s * 0.14f, color);
    Circle(hx, top + bowl_h * 0.35f, s * 0.075f, 0xFF101828);
  }
  RoundRect(int(cx - bowl_w / 2), int(top), int(bowl_w), int(bowl_h * 0.6f), int(s * 0.04f), color);
  Circle(cx, top + bowl_h * 0.45f, bowl_w / 2, color);
  RoundRect(int(cx - s * 0.05f), int(top + bowl_h * 0.9f), int(s * 0.1f), int(s * 0.22f), 2, color);
  RoundRect(int(cx - s * 0.22f), int(top + bowl_h * 0.9f + s * 0.2f), int(s * 0.44f), int(s * 0.12f), int(s * 0.04f),
            color);
  // a highlight
  Circle(cx - bowl_w * 0.18f, top + bowl_h * 0.3f, s * 0.06f, 0x70FFFFFF);
}

void Check(float cx, float cy, float s, screen::Color color) {
  screen::Line(cx - s * 0.45f, cy, cx - s * 0.12f, cy + s * 0.32f, s * 0.2f, color);
  screen::Line(cx - s * 0.12f, cy + s * 0.32f, cx + s * 0.5f, cy - s * 0.35f, s * 0.2f, color);
}

// A bell (struck through when off).
void Bell(float cx, float cy, float s, screen::Color color, bool off) {
  using namespace screen;
  Circle(cx, cy - s * 0.12f, s * 0.32f, color);
  RoundRect(int(cx - s * 0.32f), int(cy - s * 0.12f), int(s * 0.64f), int(s * 0.36f), 2, color);
  RoundRect(int(cx - s * 0.46f), int(cy + s * 0.2f), int(s * 0.92f), int(s * 0.1f), 2, color);
  Circle(cx, cy + s * 0.38f, s * 0.1f, color);
  if (off) Line(cx - s * 0.5f, cy - s * 0.5f, cx + s * 0.5f, cy + s * 0.5f, s * 0.12f, color);
}

// A cup of coffee with a heart on it (as Ko-fi's), steam over it; `s` tall.
void CoffeeCup(float cx, float cy, float s, float t) {
  using namespace screen;
  const screen::Color cup = 0xFFF5F0E8, rim = 0xFFC8BEB2, red = 0xFFFF4848;
  const float w = s * 0.95f, h = s * 0.8f;
  const float top = cy - h / 2 + s * 0.1f;
  Circle(cx + w / 2 + s * 0.02f, cy + s * 0.12f, s * 0.22f, cup);
  Circle(cx + w / 2 + s * 0.02f, cy + s * 0.12f, s * 0.12f, 0xFF16213A);
  RoundRect(int(cx - w / 2), int(top), int(w), int(h), int(s * 0.2f), cup);
  RoundRect(int(cx - w / 2), int(top), int(w), int(s * 0.12f), 2, cup);
  FillRect(int(cx - w / 2), int(top), int(w), int(s * 0.07f), rim);
  RoundRect(int(cx - w * 0.7f), int(top + h + s * 0.02f), int(w * 1.4f), int(s * 0.08f), int(s * 0.04f), rim);
  // the heart, beating
  const float r = s * 0.12f * (1.0f + 0.08f * std::max(0.0f, std::sin(t * 4.0f)));
  const float hy = cy + s * 0.12f;
  Circle(cx - r * 0.78f, hy - r * 0.35f, r, red);
  Circle(cx + r * 0.78f, hy - r * 0.35f, r, red);
  for (float k = 0; k <= 1.0f; k += 0.08f) {
    Line(cx - r * 1.6f * (1 - k), hy - r * 0.1f + k * r * 1.6f, cx + r * 1.6f * (1 - k), hy - r * 0.1f + k * r * 1.6f,
         r * 0.3f, red);
  }
  // steam
  for (int i = -1; i <= 1; ++i) {
    const float x = cx + i * w * 0.25f;
    float lx = x, ly = top - s * 0.06f;
    for (int k = 1; k <= 6; ++k) {
      const float nx = x + std::sin(k * 1.3f + t * 3.0f + i) * s * 0.05f, ny = ly - s * 0.06f;
      Line(lx, ly, nx, ny, s * 0.045f, 0x90EBE6DC);
      lx = nx, ly = ny;
    }
  }
}

// A QR code on a white card, with a quiet zone of 3 modules.
void Qr(int x, int y, int size, const char* text) {
  using namespace screen;
  const qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text, qrcodegen::QrCode::Ecc::MEDIUM);
  const int modules = qr.getSize();
  const float module = float(size) / float(modules + 6);
  RoundRect(x, y, size, size, 14, 0xFFFFFFFF);
  for (int my = 0; my < modules; ++my) {
    for (int mx = 0; mx < modules; ++mx) {
      if (!qr.getModule(mx, my)) continue;
      const int x0 = x + int((mx + 3) * module), y0 = y + int((my + 3) * module);
      const int x1 = x + int((mx + 4) * module), y1 = y + int((my + 4) * module);
      FillRect(x0, y0, x1 - x0, y1 - y0, 0xFF000000);
    }
  }
}

void Placeholder(int x, int y, int size) {
  screen::RoundRectGradient(x, y, size, size, size / 6, 0xFF26344F, 0xFF162036);
  Cup(x + size / 2.0f, y + size / 2.0f, size * 0.5f, 0xFF3E4C68);
}

// The list's rows: a section's title, then its games (installed games, then
// disc images).
struct Row {
  int entry;    // -1: a section title
  int section;  // 0 installed games, 1 an emulator's, 2 disc images
};

int SectionOf(const Entry& e) { return e.installed ? 0 : e.emulator ? 1 : 2; }
constexpr int kListTop = 196, kListBottom = 960, kTitleHeight = 60, kCardHeight = 138;

std::vector<Row> Rows(const std::vector<Entry>& images) {
  std::vector<Row> rows;
  for (int section = 0; section < 3; ++section) {
    bool titled = false;
    for (size_t i = 0; i < images.size(); ++i) {
      if (SectionOf(images[i]) != section) continue;
      if (!titled) rows.push_back({-1, section}), titled = true;
      rows.push_back({int(i), section});
    }
  }
  return rows;
}

int RowHeight(const Row& r) { return r.entry < 0 ? kTitleHeight : kCardHeight; }

// The first row shown so that the selected game's is (and, when it's the
// first of its section, its title).
int FirstRow(const std::vector<Row>& rows, int selected, int first) {
  int at = 0;
  while (at < int(rows.size()) && rows[size_t(at)].entry != selected) ++at;
  if (at == int(rows.size())) return 0;
  const int want = at > 0 && rows[size_t(at - 1)].entry < 0 ? at - 1 : at;
  first = std::min(first, want);
  // (down: until the selected one fits)
  for (;;) {
    int y = kListTop;
    for (int r = first; r <= at; ++r) y += RowHeight(rows[size_t(r)]);
    if (y <= kListBottom || first >= want) break;
    ++first;
  }
  return std::max(first, 0);
}

void DrawSectionTitle(int y, int section, size_t count) {
  using namespace screen;
  const std::string title = section == 0   ? T("Installed games", "Jogos instalados")
                            : section == 1 ? T("Xbox 360 emulator (PS5X360)", "Emulador de Xbox 360 (PS5X360)")
                                           : T("Disc images", "Imagens de disco");
  const int w = Text(112, y + 14, title, 30, kText, true);
  char n[16];
  std::snprintf(n, sizeof(n), "%zu", count);
  const int nw = TextWidth(n, 22, true) + 24;
  RoundRect(112 + w + 16, y + 16, nw, 34, 17, 0x30FFFFFF);
  Text(112 + w + 28, y + 19, n, 22, kDim, true);
}

void DrawCard(const Entry& e, int y, bool on) {
  using namespace screen;
  const int x = 110, w = 1680, h = kCardHeight - 16;
  if (on) {
    Shadow(x, y, w, h, 24, 26, 0x903C82F6);
    RoundRectGradient(x, y, w, h, 24, 0xFF24406F, 0xFF1A2E52);
    RoundFrame(x, y, w, h, 24, 2, 0xC079A8FF);
  } else {
    RoundRect(x, y, w, h, 24, 0x9C131D31);
  }
  const int icon = 94, ix = x + 16, iy = y + 14;
  if (!e.icon.empty()) {
    Image(ix, iy, icon, icon, e.icon.pixels, e.icon.w, e.icon.h, 18);
  } else {
    Placeholder(ix, iy, icon);
  }
  const std::string file = std::filesystem::path(e.path).filename().string();
  const std::string title = e.read && e.ok          ? e.name
                            : e.installed             ? AppOf(e.path)
                            : e.emulator && e.size == 0 ? std::filesystem::path(e.path).parent_path().filename().string()
                                                        : file;
  Text(x + 136, y + 18, Fit(title, 34, 1180, true), 34, on ? kText : 0xFFDCE2EC, true);
  std::string details;
  if (!e.read) {
    details = T("Reading the game...", "Lendo o jogo...");
  } else if (!e.ok) {
    details = T("No achievements to read in this game", "Este jogo não tem conquistas para ler");
  } else {
    char d[96];
    std::snprintf(d, sizeof(d), T("%zu achievements", "%zu conquistas"), e.achievements);
    details = d;
  }
  if (e.installed) {
    details += std::string("   ·   ") + T("Installed", "Instalado") + " (" + AppOf(e.path) + ")";
  } else if (e.emulator) {
    const std::filesystem::path p(e.path);
    const std::string folder = (p.extension() == ".xex" || p.extension() == ".XEX")
                                   ? p.parent_path().filename().string()
                                   : p.filename().string();
    details += "   ·   " + Fit(folder, 22, 640) + (e.size ? "   ·   " + Size(e.size) : "");
  } else {
    details += "   ·   " + Fit(file, 22, 640) + "   ·   " + Size(e.size);
  }
  Text(x + 136, y + 70, Fit(details, 22, 1220), 22, kDim);
  // its state: in the trophy list already, or new
  if (e.read && e.ok) {
    const bool made = work.done.count(e.set_id) > 0;
    const std::string badge = made ? T("On the PS5", "No PS5") : T("New", "Novo");
    const int bw = TextWidth(badge, 22, true) + (made ? 64 : 40);
    const int bx = x + w - bw - 28, by = y + 40;
    RoundRect(bx, by, bw, 42, 21, made ? 0x334CC38A : 0x333C82F6);
    if (made) Check(float(bx + 26), float(by + 21), 18, kGreen);
    Text(bx + (made ? 46 : 20), by + 7, badge, 22, made ? kGreen : kAccentLight, true);
    // its native notifications: on, off (Square), or the port's own
    if (made && e.notifications != Notifications::kNone) {
      const bool on = e.notifications == Notifications::kOn, native = e.notifications == Notifications::kNative;
      const std::string label = native ? T("Native notifications (port)", "Notificações nativas (port)")
                                : on   ? T("Notifications on", "Notificações ligadas")
                                       : T("Notifications off", "Notificações desligadas");
      const int lw = TextWidth(label, 20, true) + 58;
      const int lx = bx - lw - 14, ly = by + 2;
      const screen::Color c = native ? kGold : on ? kAccentLight : kFaint;
      RoundRect(lx, ly, lw, 38, 19, native ? 0x30F2C94C : on ? 0x2A3C82F6 : 0x22FFFFFF);
      Bell(float(lx + 24), float(ly + 19), 18, c, !on && !native);
      Text(lx + 42, ly + 7, label, 20, c, true);
    }
  }
}

void DrawList(int selected, int& first) {
  using namespace screen;
  std::lock_guard<std::mutex> lock(work.mutex);
  const auto& images = work.images;
  if (images.empty()) {
    RoundRect(110, 230, 1700, 220, 28, 0xB0121C30);
    Text(160, 270, T("No game found", "Nenhum jogo encontrado"), 40, kText, true);
    Text(160, 336, T("Put the game's .iso in /data, on a USB drive or the extended storage, then search again.",
                     "Coloque o .iso do jogo em /data, num pendrive ou no armazenamento estendido e procure de novo."),
         28, kDim);
    Hints({{Glyph::kTriangle, T("Search again", "Procurar de novo")}});
    return;
  }
  const std::vector<Row> rows = Rows(images);
  first = FirstRow(rows, selected, first);
  size_t counts[3] = {};
  for (const Entry& e : images) ++counts[SectionOf(e)];
  int y = kListTop;
  for (int r = first; r < int(rows.size()); ++r) {
    const Row& row = rows[size_t(r)];
    if (y + RowHeight(row) - 16 > kListBottom) break;
    if (row.entry < 0) {
      DrawSectionTitle(y, row.section, counts[row.section]);
    } else {
      DrawCard(images[size_t(row.entry)], y, row.entry == selected);
    }
    y += RowHeight(row);
  }
  // a scroll bar
  int total = 0, before = 0;
  for (int r = 0; r < int(rows.size()); ++r) {
    total += RowHeight(rows[size_t(r)]);
    if (r < first) before += RowHeight(rows[size_t(r)]);
  }
  const int track = kListBottom - kListTop;
  if (total > track) {
    const int bar = std::max(60, track * track / total);
    const int at = std::min(track - bar, (track - bar) * before / std::max(1, total - track));
    RoundRect(1808, kListTop, 6, track, 3, 0x20FFFFFF);
    RoundRect(1808, kListTop + at, 6, bar, 3, 0x90FFFFFF);
  }
  const Entry& on = images[size_t(std::clamp(selected, 0, int(images.size()) - 1))];
  if (on.notifications == Notifications::kOn || on.notifications == Notifications::kOff) {
    Hints({{Glyph::kCross, T("Make the trophies", "Criar os troféus")},
           {Glyph::kSquare, on.notifications == Notifications::kOn ? T("Notifications off", "Desligar notificações")
                                                                    : T("Notifications on", "Ligar notificações")},
           {Glyph::kTriangle, T("Search again", "Procurar de novo")}});
  } else {
    Hints({{Glyph::kCross, T("Make the trophies", "Criar os troféus")},
           {Glyph::kTriangle, T("Search again", "Procurar de novo")}});
  }
}

// The tabs, at the header's right (L1 / R1 change them).
void DrawTabs(int tab) {
  using namespace screen;
  const char* names[2] = {T("Games", "Jogos"), T("Donate", "Doar")};
  int x = 1810;
  int widths[2];
  for (int i = 1; i >= 0; --i) {
    widths[i] = TextWidth(names[i], 26, true) + 56;
    x -= widths[i] + (i ? 0 : 12);
  }
  x += 0;
  Text(x - 64, 82, "L1", 22, kFaint, true);
  for (int i = 0; i < 2; ++i) {
    const bool on = i == tab;
    if (on) RoundRectGradient(x, 70, widths[i], 54, 27, 0xFF2F5FB0, 0xFF234A8C);
    else RoundRect(x, 70, widths[i], 54, 27, 0x30FFFFFF);
    Text(x + 28, 82, names[i], 26, on ? kText : kDim, true);
    x += widths[i] + 12;
  }
  Text(x + 4, 82, "R1", 22, kFaint, true);
}

void DrawDonate(float t) {
  using namespace screen;
  const int x = 210, y = 200, w = 1500, h = 700;
  Shadow(x, y, w, h, 36, 40, 0x90000000);
  RoundRectGradient(x, y, w, h, 36, 0xF01C2B4A, 0xF0101828);
  Glow(float(x + 260), float(y + 240), 380, 0x30FF4848);
  // who made it, and the cup
  Text(x + 80, y + 70, T("MADE BY LAMARKING", "FEITO POR LAMARKING"), 44, kGold, true);
  const char* lines[] = {
      T("Enjoying it? Consider a donation to support the ports", "Gostou? Considere fazer uma doação para apoiar os ports"),
      T("and keep the server online.", "e manter o servidor online.")};
  Text(x + 80, y + 140, lines[0], 30, kText);
  Text(x + 80, y + 182, lines[1], 30, kText);
  CoffeeCup(float(x + 140), float(y + 330), 110, t);
  Text(x + 240, y + 282, "KO-FI", 30, kGold, true);
  Text(x + 240, y + 326, "ko-fi.com/lamarking", 40, kText, true);
  // the community
  Text(x + 80, y + 456, T("Join the community on Discord:", "Entre na comunidade no Discord:"), 28, kDim);
  Text(x + 80, y + 498, "discord.gg/pmK6zDYQSP", 40, kAccentLight, true);
  Text(x + 80, y + 590, T("Trophy file research thanks to Trophy Manager (kauansilvalopes) and KIK-Trophy.",
                          "Pesquisa dos arquivos de troféu: Trophy Manager (kauansilvalopes) e KIK-Trophy."),
       20, kFaint);
  // the QR code of the Discord
  Qr(x + w - 400, y + 130, 320, "https://discord.gg/pmK6zDYQSP");
  const std::string scan = T("Scan for the Discord", "Aponte a câmera para o Discord");
  Text(x + w - 240 - TextWidth(scan, 24) / 2, y + 476, scan, 24, kDim);
  Hints({{Glyph::kCircle, T("Back to the games", "Voltar aos jogos")}});
}

void DrawWorking(float t) {
  using namespace screen;
  std::lock_guard<std::mutex> lock(work.mutex);
  const int x = 360, y = 230, w = 1200, h = 560;
  Shadow(x, y, w, h, 36, 40, 0x90000000);
  RoundRectGradient(x, y, w, h, 36, 0xF01A2846, 0xF0101828);
  if (!work.working_icon.empty()) {
    Image(x + 60, y + 56, 120, 120, work.working_icon.pixels, work.working_icon.w, work.working_icon.h, 24);
  } else {
    Placeholder(x + 60, y + 56, 120);
  }
  Text(x + 210, y + 70, Fit(work.working_name, 40, w - 260, true), 40, kText, true);
  Text(x + 212, y + 128, T("Making its PS5 trophies", "Criando os troféus de PS5"), 26, kDim);
  static const char* const kSteps[2][3] = {
      {"Reading the game's achievements", "Making the trophies", "Installing them on the PS5"},
      {"Lendo as conquistas do jogo", "Criando os troféus", "Instalando no PS5"}};
  const int step = work.step;
  for (int i = 0; i < 3; ++i) {
    const float cy = float(y + 252 + i * 74);
    const float cx = float(x + 84);
    if (i < step) {
      Circle(cx, cy, 20, 0xFF2E9E6A);
      Check(cx, cy, 20, kText);
    } else if (i == step) {
      Circle(cx, cy, 20, kAccent);
      // a turning dot
      Circle(cx + 9 * std::cos(t * 6), cy + 9 * std::sin(t * 6), 4.5f, kText);
    } else {
      Circle(cx, cy, 20, 0xFF26344F);
    }
    Text(x + 128, int(cy) - 20, kSteps[text::Portuguese()][i], 32, i <= step ? kText : kFaint, i == step);
  }
  const float v = std::clamp((step + work.progress.load()) / 3.0f, 0.0f, 1.0f);
  RoundRect(x + 60, y + 476, w - 120, 16, 8, 0x30FFFFFF);
  if (v > 0) RoundRectGradient(x + 60, y + 476, std::max(16, int((w - 120) * v)), 16, 8, 0xFF79A8FF, 0xFF3C82F6);
  Text(x + 60, y + 504, T("This takes a moment; don't close the app.", "Leva um instante; não feche o app."), 22, kDim);
}

void DrawDone() {
  using namespace screen;
  std::lock_guard<std::mutex> lock(work.mutex);
  const int x = 260, y = 230, w = 1400, h = 560;
  Shadow(x, y, w, h, 36, 40, 0x90000000);
  RoundRectGradient(x, y, w, h, 36, 0xF01C2B4A, 0xF0101828);
  Glow(float(x + 200), float(y + 200), 360, 0x40F2C94C);
  if (!work.icon.empty()) {
    Shadow(x + 70, y + 70, 260, 260, 40, 28, 0xA0000000);
    Image(x + 70, y + 70, 260, 260, work.icon.pixels, work.icon.w, work.icon.h, 40);
  } else {
    Placeholder(x + 70, y + 70, 260);
  }
  Circle(float(x + 312), float(y + 312), 34, 0xFF2E9E6A);
  Check(float(x + 312), float(y + 312), 34, kText);
  const int tx = x + 380;
  Text(tx, y + 76, work.updated ? T("Trophies updated", "Troféus atualizados") : T("Trophies ready", "Troféus prontos"),
       28, kGold, true);
  Text(tx, y + 118, Fit(work.title, 48, w - 440, true), 48, kText, true);
  char line[220];
  std::snprintf(line, sizeof(line), T("%zu trophies in the PS5's trophy list", "%zu troféus na lista de troféus do PS5"),
                work.trophies);
  Text(tx, y + 190, line, 28, kDim);
  // the cups by grade
  static const char* const kGradeNames[2][5] = {{"", "Platinum", "Gold", "Silver", "Bronze"},
                                                {"", "Platina", "Ouro", "Prata", "Bronze"}};
  for (int g = 1; g <= 4; ++g) {
    const int cx = tx + (g - 1) * 230;
    RoundRect(cx, y + 252, 206, 132, 22, 0x50000000);
    Cup(float(cx + 52), float(y + 318), 66, kGradeColors[g]);
    char n[16];
    std::snprintf(n, sizeof(n), "%d", work.grades[g]);
    Text(cx + 100, y + 270, n, 44, kText, true);
    Text(cx + 100, y + 328, kGradeNames[text::Portuguese()][g], 22, kDim);
  }
  if (work.imported) {
    const bool one = work.imported == 1;
    if (!work.port_name.empty()) {
      std::snprintf(line, sizeof(line),
                    one ? T("%zu achievement earned already was brought in (%s).", "%zu conquista já ganha foi trazida (%s).")
                        : T("%zu achievements earned already were brought in (%s).",
                            "%zu conquistas já ganhas foram trazidas (%s)."),
                    work.imported, work.port_name.c_str());
    } else {
      std::snprintf(line, sizeof(line),
                    one ? T("%zu achievement earned already was brought in.", "%zu conquista já ganha foi trazida.")
                        : T("%zu achievements earned already were brought in.", "%zu conquistas já ganhas foram trazidas."),
                    work.imported);
    }
    Text(tx, y + 424, Fit(line, 28, w - 440), 28, kText);
  } else {
    Text(tx, y + 424, T("No achievement earned yet in this game on the console.",
                        "Nenhuma conquista ganha ainda neste jogo no console."), 28, kDim);
  }
  if (work.native) {
    Text(tx, y + 462, T("This game's port shows its own native trophy notifications.",
                        "O port deste jogo mostra as próprias notificações nativas de troféu."), 22, kGold);
  } else if (work.watching) {
    Text(tx, y + 462, T("Native trophy notifications are on for this game: earn an achievement and it pops up.",
                        "As notificações nativas de troféu estão ligadas para este jogo."), 22, kGreen);
  }
  Text(tx, y + 498, T("Open Trophies on the PS5 to see them. If they're not there yet, restart the console.",
                      "Abra Troféus no PS5 para vê-los. Se ainda não aparecerem, reinicie o console."),
       22, kDim);
  Hints({{Glyph::kCircle, T("Back", "Voltar")}});
}

void DrawError() {
  using namespace screen;
  std::lock_guard<std::mutex> lock(work.mutex);
  const int x = 360, y = 290, w = 1200, h = 360;
  Shadow(x, y, w, h, 36, 40, 0x90000000);
  RoundRectGradient(x, y, w, h, 36, 0xF0301A24, 0xF0181018);
  Circle(float(x + 100), float(y + 100), 40, 0xFFB84444);
  Line(float(x + 100), float(y + 78), float(x + 100), float(y + 106), 7, kText);
  Circle(float(x + 100), float(y + 122), 4.5f, kText);
  Text(x + 170, y + 74, T("The trophies couldn't be made", "Não foi possível criar os troféus"), 38, kText, true);
  // the message, in lines that fit
  std::string rest = work.error;
  int ly = y + 170;
  while (!rest.empty() && ly < y + h - 60) {
    size_t cut = rest.size();
    while (TextWidth(rest.substr(0, cut), 28) > w - 240) {
      const size_t space = rest.rfind(' ', cut - 1);
      if (space == std::string::npos || space == 0) break;
      cut = space;
    }
    Text(x + 170, ly, rest.substr(0, cut), 28, 0xFFE8D6DA);
    rest = cut < rest.size() ? rest.substr(cut + 1) : "";
    ly += 42;
  }
  Hints({{Glyph::kCircle, T("Back", "Voltar")}});
}

void DrawSearching(float t) {
  using namespace screen;
  const float cx = 960, cy = 470;
  for (int i = 0; i < 12; ++i) {
    const float a = float(i) / 12 * 6.2832f;
    const float fade = std::fmod(t * 1.6f - float(i) / 12 + 2, 1.0f);
    Circle(cx + 46 * std::cos(a), cy + 46 * std::sin(a), 7, screen::Color(uint32_t(60 + 195 * (1 - fade)) << 24) | 0x79A8FF);
  }
  const std::string a = T("Looking for games (.iso and installed ports)", "Procurando jogos (.iso e ports instalados)");
  Text(int(cx) - TextWidth(a, 36, true) / 2, 570, a, 36, kText, true);
  const std::string b = T("On the console's storage, USB drives and extended storage",
                          "No armazenamento do console, em pendrives e no armazenamento estendido");
  Text(int(cx) - TextWidth(b, 26) / 2, 626, b, 26, kDim);
}

}  // namespace

int main() {
  if (!screen::Open("/app0/fonts/NotoSans-Regular.ttf", "/app0/fonts/NotoSans-Bold.ttf")) return 1;
  input::Open();
  // (the native notifications of the games made: the watcher, when not running)
  // (and in the Payload Manager's autoload list, for the console's next starts)
  std::thread([] {
    install::AddToAutoload();
    install::StartWatcher();
  }).detach();
  app_icon = Decode(ReadFile("/app0/sce_sys/icon0.png"));
  MakeBackground();
  StartSearch();
  int selected = 0, top = 0, tab = 0;
  int drawn_changes = -1;
  State drawn_state = State::kError;
  const auto started = std::chrono::steady_clock::now();
  for (;;) {
    const uint32_t pressed = input::Pressed();
    const State state = work.state;
    if (state == State::kList || state == State::kSearching) {
      if (pressed & input::kR1) tab = std::min(tab + 1, 1);
      if (pressed & input::kL1) tab = std::max(tab - 1, 0);
      if (tab == 1 && (pressed & input::kCircle)) tab = 0;
    }
    if (tab == 1) {
      // (the Donate tab: nothing else to do)
    } else if (state == State::kList) {
      size_t count;
      {
        std::lock_guard<std::mutex> lock(work.mutex);
        count = work.images.size();
      }
      if (count) {
        // (in the list's order: the installed games, then the disc images)
        std::vector<int> order;
        {
          std::lock_guard<std::mutex> lock(work.mutex);
          for (const Row& r : Rows(work.images)) {
            if (r.entry >= 0) order.push_back(r.entry);
          }
        }
        int at = int(std::find(order.begin(), order.end(), selected) - order.begin());
        if (at >= int(order.size())) at = 0;
        if (pressed & input::kDown) at = std::min(at + 1, int(order.size()) - 1);
        if (pressed & input::kUp) at = std::max(at - 1, 0);
        selected = order.empty() ? 0 : order[size_t(at)];
        if (pressed & input::kSquare) {
          std::lock_guard<std::mutex> lock(work.mutex);
          Entry& e = work.images[size_t(selected)];
          if (e.notifications == Notifications::kOn || e.notifications == Notifications::kOff) {
            SetNotifications(e.set_id, e.notifications == Notifications::kOff);
            e.notifications = NotificationsOf(e.set_id);
            ++work.changes;
          }
        }
        if (pressed & input::kCross) {
          Entry entry;
          {
            std::lock_guard<std::mutex> lock(work.mutex);
            entry = work.images[size_t(selected)];
          }
          Make(entry);
        }
      }
      if (pressed & input::kTriangle) StartSearch();
    } else if (state == State::kDone || state == State::kError) {
      if (pressed & (input::kCircle | input::kCross)) {
        work.state = State::kList;
        ++work.changes;
      }
    }
    // drawn again when something changed (or it moves)
    const bool animated = state == State::kSearching || state == State::kWorking || tab == 1;
    const int changes = work.changes;
    if (!pressed && !animated && changes == drawn_changes && state == drawn_state) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      continue;
    }
    drawn_changes = changes, drawn_state = work.state;
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
    screen::Restore();
    const bool tabs = work.state == State::kList || work.state == State::kSearching;
    if (tabs) DrawTabs(tab);
    if (tabs && tab == 1) {
      DrawDonate(t);
      screen::Present();
      continue;
    }
    switch (work.state.load()) {
      case State::kSearching: DrawSearching(t); break;
      case State::kList: DrawList(selected, top); break;
      case State::kWorking: DrawWorking(t); break;
      case State::kDone: DrawDone(); break;
      case State::kError: DrawError(); break;
    }
    screen::Present();
  }
}
