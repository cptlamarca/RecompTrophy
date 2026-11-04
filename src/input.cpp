// See input.h.

#include "input.h"

#include <chrono>
#include <cstring>

extern "C" {
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t type, int32_t index, const void* param);
int scePadGetHandle(int32_t user_id, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void* data);
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(int32_t* user);
int sceUserServiceGetForegroundUser(int32_t* user);
}

namespace input {
namespace {

struct PadData {
  uint32_t buttons;
  uint8_t sticks[4];
  uint8_t l2, r2;
  uint16_t padding;
  uint8_t rest[240];
};

int32_t user = -1;
int32_t handle = -1;
uint32_t held = 0;
std::chrono::steady_clock::time_point repeat_at;

}  // namespace

void Open() {
  sceUserServiceInitialize(nullptr);
  if (sceUserServiceGetInitialUser(&user) != 0 || user < 0) sceUserServiceGetForegroundUser(&user);
  scePadInit();
  handle = scePadOpen(user, 0, 0, nullptr);
  if (handle < 0) handle = scePadGetHandle(user, 0, 0);
}

int32_t User() { return user; }

uint32_t Pressed() {
  if (handle < 0) return 0;
  PadData data;
  std::memset(&data, 0, sizeof(data));
  if (scePadReadState(handle, &data) != 0) return 0;
  uint32_t b = data.buttons;
  // the left stick as the d-pad
  if (data.sticks[1] < 40) b |= kUp;
  if (data.sticks[1] > 215) b |= kDown;
  if (data.sticks[0] < 40) b |= kLeft;
  if (data.sticks[0] > 215) b |= kRight;
  const auto now = std::chrono::steady_clock::now();
  uint32_t pressed = b & ~held;
  constexpr uint32_t kRepeating = kUp | kDown | kLeft | kRight;
  if (pressed & kRepeating) {
    repeat_at = now + std::chrono::milliseconds(400);
  } else if ((b & held & kRepeating) && now >= repeat_at) {
    pressed |= b & held & kRepeating;
    repeat_at = now + std::chrono::milliseconds(90);
  }
  held = b;
  return pressed;
}

}  // namespace input
