// Fable 2: scripted controller input for unattended tests (tools/smoke_test.py).
//
// The keyboard-as-controller driver only sees keys while the game window has
// focus, so tests that send key presses take over the user's desktop. Instead,
// a test writes a script to "input_now" next to the executable; the game reads
// it (and deletes it) on its next XamInputGetState call and plays it back as
// pad 0, whatever window is in front.
//
// Script: whitespace-separated items NAME:holdMs[:pauseMs] (pause defaults to
// 150 ms), played one after another; a new script starts when the current one
// ends. NAME is a button (A B X Y START BACK LB RB LT RT LS RS DUP DDOWN DLEFT
// DRIGHT), a full stick deflection (LX+ LX- LY+ LY- RX+ RX- RY+ RY-), several of
// those held together joined by '&' (LY+&RX+), WAIT, or
// DISCONNECT (pad 0 reports not connected, as when the window loses focus
// with the keyboard driver).
//
// Once a script has been received, pad 0 always reports connected (with the
// real input merged in), so the game never pauses for a lost controller while
// its window is in the background.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xtypes.h>

#include "core/logging.h"

using rex::X_RESULT;

namespace {

using Clock = std::chrono::steady_clock;

struct Segment {
  int64_t startMs = 0, endMs = 0;
  uint16_t buttons = 0;
  uint8_t lt = 0, rt = 0;
  int16_t axis[4] = {};  // lx, ly, rx, ry
  bool disconnect = false;
};

struct Player {
  std::mutex mutex;
  bool armed = false;
  Clock::time_point epoch = Clock::now();
  Clock::time_point lastPoll{};
  int64_t endMs = 0;
  std::vector<Segment> segments;
  uint32_t changes = 0;
  uint64_t lastKey = 0;
};

Player &player() {
  static Player p;
  return p;
}

int64_t NowMs(const Player &p) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - p.epoch).count();
}

// One input name applied to seg (buttons accumulate, a stick sets its axis).
bool ApplyName(const std::string &name, Segment &seg) {
  struct Button {
    const char *name;
    uint16_t bit;
  };
  static const Button kButtons[] = {
      {"DUP", 0x0001}, {"DDOWN", 0x0002}, {"DLEFT", 0x0004}, {"DRIGHT", 0x0008}, {"START", 0x0010},
      {"BACK", 0x0020}, {"LS", 0x0040}, {"RS", 0x0080}, {"LB", 0x0100}, {"RB", 0x0200},
      {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000},
  };
  for (const Button &b : kButtons) {
    if (name == b.name) {
      seg.buttons |= b.bit;
      return true;
    }
  }
  if (name == "LT" || name == "RT") {
    (name == "LT" ? seg.lt : seg.rt) = 255;
    return true;
  }
  if (name == "WAIT")
    return true;
  if (name == "DISCONNECT") {
    seg.disconnect = true;  // pad 0 reports not connected
    return true;
  }
  if (name.size() == 3 && (name[0] == 'L' || name[0] == 'R') && (name[1] == 'X' || name[1] == 'Y') &&
      (name[2] == '+' || name[2] == '-')) {
    const int index = (name[0] == 'R' ? 2 : 0) + (name[1] == 'Y' ? 1 : 0);
    seg.axis[index] = name[2] == '+' ? 32767 : -32768;
    return true;
  }
  return false;
}

// NAME may combine inputs held together with '&' (LY+&RX+: walk while turning).
bool ParseItem(const std::string &item, Segment &seg, int64_t &hold, int64_t &pause) {
  const size_t c1 = item.find(':');
  if (c1 == std::string::npos)
    return false;
  const std::string name = item.substr(0, c1);
  const size_t c2 = item.find(':', c1 + 1);
  hold = std::stoll(item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1));
  pause = c2 == std::string::npos ? 150 : std::stoll(item.substr(c2 + 1));
  size_t start = 0;
  while (start <= name.size()) {
    const size_t amp = name.find('&', start);
    const std::string part = name.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
    if (part.empty() || !ApplyName(part, seg))
      return false;
    if (amp == std::string::npos)
      break;
    start = amp + 1;
  }
  return true;
}

void PollScriptLocked(Player &p) {
  const auto now = Clock::now();
  if (now - p.lastPoll < std::chrono::milliseconds(50))
    return;
  p.lastPoll = now;
  std::error_code ec;
  const auto path = rex::filesystem::GetExecutableFolder() / "input_now";
  if (!std::filesystem::exists(path, ec))
    return;
  std::string text;
  {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    text = ss.str();
  }
  std::filesystem::remove(path, ec);
  const int64_t t = NowMs(p);
  int64_t at = std::max(t, p.endMs);
  std::istringstream items(text);
  std::string item;
  uint32_t count = 0;
  while (items >> item) {
    Segment seg;
    int64_t hold = 0, pause = 0;
    try {
      if (!ParseItem(item, seg, hold, pause)) {
        EOT_WARN("[input] unknown script item '{}'", item);
        continue;
      }
    } catch (...) {
      EOT_WARN("[input] bad script item '{}'", item);
      continue;
    }
    seg.startMs = at;
    seg.endMs = at + hold;
    p.segments.push_back(seg);
    at = seg.endMs + pause;
    ++count;
  }
  p.endMs = at;
  p.armed = true;
  EOT_INFO("[input] script: {} items, {} ms", count, at - t);
}

}  // namespace

extern "C" REX_FUNC(__imp__XamInputGetState) {
  const uint32_t user_index = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  const uint32_t state_va = ctx.r5.u32;
  (void)user_index;
  constexpr uint32_t kFlagGamepad = 0x01;
  if ((flags & 0xFF) && (flags & kFlagGamepad) == 0) {
    ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED;
    return;
  }
  (void)base;
  auto *state = state_va ? REX_KERNEL_MEMORY()->TranslateVirtual<rex::input::X_INPUT_STATE *>(state_va) : nullptr;
  auto *is = static_cast<rex::input::InputSystem *>(REX_KERNEL_STATE()->emulator()->input_system());
  // Pad 0 only, as the SDK pins every user to 0.
  X_RESULT result = is ? is->GetState(0, state) : X_ERROR_DEVICE_NOT_CONNECTED;

  Player &p = player();
  std::lock_guard lock(p.mutex);
  PollScriptLocked(p);
  if (!p.armed) {
    ctx.r3.u64 = result;
    return;
  }
  const int64_t t = NowMs(p);
  while (!p.segments.empty() && p.segments.front().endMs <= t)
    p.segments.erase(p.segments.begin());
  Segment cur;
  for (const Segment &s : p.segments) {
    if (s.startMs > t)
      break;
    cur = s;
  }
  if (cur.disconnect) {
    ctx.r3.u64 = X_ERROR_DEVICE_NOT_CONNECTED;
    return;
  }
  if (state) {
    if (result != X_ERROR_SUCCESS)
      std::memset(static_cast<void *>(state), 0, sizeof(*state));
    auto &g = state->gamepad;
    g.buttons = static_cast<uint16_t>(static_cast<uint16_t>(g.buttons) | cur.buttons);
    g.left_trigger = std::max(g.left_trigger, cur.lt);
    g.right_trigger = std::max(g.right_trigger, cur.rt);
    if (cur.axis[0]) g.thumb_lx = cur.axis[0];
    if (cur.axis[1]) g.thumb_ly = cur.axis[1];
    if (cur.axis[2]) g.thumb_rx = cur.axis[2];
    if (cur.axis[3]) g.thumb_ry = cur.axis[3];
    const uint64_t key = (uint64_t(cur.buttons) << 48) ^ (uint64_t(uint16_t(cur.axis[0])) << 32) ^
                         (uint64_t(uint16_t(cur.axis[1])) << 16) ^ uint16_t(cur.axis[2]) ^
                         (uint64_t(uint16_t(cur.axis[3])) << 8) ^ (uint64_t(cur.lt) << 40) ^ (uint64_t(cur.rt) << 24);
    if (key != p.lastKey) {
      p.lastKey = key;
      ++p.changes;
    }
    state->packet_number = static_cast<uint32_t>(state->packet_number) + (p.changes << 16);
  }
  ctx.r3.u64 = X_ERROR_SUCCESS;
}
