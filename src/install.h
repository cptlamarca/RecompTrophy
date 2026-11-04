// What goes with a game's trophies: its port installed on the console, when
// there is one (its name, icon and the achievements already earned in it),
// and the installation itself, done by the relay payload (payload/relay.c)
// sent to the payload loader on port 9021, as an app can't write where the
// console keeps trophies.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace install {

constexpr char kJobFolder[] = "/data/trophies360/job";

struct Port {
  std::string folder;                 // /data/homebrew/TITLE
  std::string name;                   // its titleName
  std::vector<uint8_t> icon;          // its sce_sys/icon0.png
  std::vector<std::pair<uint32_t, int64_t>> earned;  // achievement, unix time
  std::vector<std::string> sources;  // the files they're kept in (.toml of ReXGlue ports, .gpd of profiles)
};

// The port of an Xbox 360 title among /data/homebrew's apps (one with its
// achievements file, UserData/achievements/TITLEID.toml, or its shaders).
bool FindPort(uint32_t title_id, Port& port);

struct Result {
  bool ok = false;
  std::string set_state;       // installed, same, failed
  std::string progress_state;  // created, updated, unchanged, no-template, write-failed, none
  std::string error;           // a message for the player when !ok
};

// The set (kJobFolder/TROPHY.UCP and trophies.txt, made already) and the
// earned achievements (all of them: the platinum too) given to the relay.
Result Run(const std::string& set_id, int32_t user, const std::vector<std::pair<uint32_t, int64_t>>& earned,
           size_t achievement_count);

// The watcher (payload/watcher.c): a payload that stays loaded after the app
// closes and, every few seconds, looks at the achievement files of the games
// whose trophies were made (kWatchFolder/SET/: game.txt, map.txt,
// sources.txt), records each new one in the player's trophy list and shows
// the console's own trophy notification for it. Sent unless one runs already
// (its heartbeat, kWatchFolder/alive, is fresh).
constexpr char kWatchFolder[] = "/data/trophies360/watch";

// What the watcher needs of a game: its set, the player, its name, each
// achievement's trophy (id, trophy, grade, name) and the files to look at.
bool Watch(const std::string& set_id, int32_t user, const std::string& name,
           const std::vector<std::pair<uint32_t, uint32_t>>& trophies, const std::vector<int>& grades,
           const std::vector<std::string>& names, const std::vector<std::string>& sources);

// The watcher sent to the loader unless it runs: true when it runs.
bool StartWatcher();

// The watcher in the Payload Manager's autoload list (/data/pldmgr): put in
// its payloads/local/recomptrophy-watcher/ here; its line in autoload.txt
// (root's only) the watcher adds itself when it starts.
bool AddToAutoload();

}  // namespace install
