// Scripted pad for automated tests. Set BF2_INPUT=<file>; each line is
//   <seconds> <BUTTON[+BUTTON...]> [hold_ms]
// with seconds measured from driver setup. Buttons: A B X Y START BACK UP DOWN
// LEFT RIGHT LB RB LT RT LS RS, or stick axes LX=<-1..1> LY=<-1..1>.
// The file is re-read when it grows, so lines can be appended while running.
// Lines starting with '#' are ignored.

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <rex/input/input_driver.h>
#include <rex/logging.h>

using rex::X_STATUS;
using rex::X_RESULT;

class ScriptInputDriver final : public rex::input::InputDriver {
 public:
  explicit ScriptInputDriver(std::string path)
      : rex::input::InputDriver(nullptr, 0), path_(std::move(path)) {}

  X_STATUS Setup() override {
    start_ = std::chrono::steady_clock::now();
    REXLOG_INFO("script input: {}", path_);
    return X_STATUS_SUCCESS;
  }

  void EnumerateDevices(std::vector<rex::input::DeviceInfo>& out) override {
    rex::input::DeviceInfo info;
    info.id = kId;
    info.name = "Script pad";
    info.guid = "script";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(rex::input::DeviceId id, rex::input::X_INPUT_STATE* out) override {
    if (id != kId) return X_ERROR_DEVICE_NOT_CONNECTED;
    Current(out);
    return X_ERROR_SUCCESS;
  }

  void Current(rex::input::X_INPUT_STATE* out) {
    Reload();
    double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (const auto& e : events_) {
        if (now < e.t || now >= e.t + e.hold) continue;
        buttons |= e.buttons;
        if (e.lt) lt = 255;
        if (e.rt) rt = 255;
        if (e.has_lx) lx = e.lx;
        if (e.has_ly) ly = e.ly;
      }
    }
    std::memset(out, 0, sizeof(*out));
    out->packet_number = ++packet_;
    out->gamepad.buttons = buttons;
    out->gamepad.left_trigger = lt;
    out->gamepad.right_trigger = rt;
    out->gamepad.thumb_lx = lx;
    out->gamepad.thumb_ly = ly;
  }

  X_RESULT GetDeviceCapabilities(rex::input::DeviceId id, uint32_t,
                                 rex::input::X_INPUT_CAPABILITIES* out) override {
    if (id != kId) return X_ERROR_DEVICE_NOT_CONNECTED;
    std::memset(out, 0, sizeof(*out));
    out->type = 0x01;
    out->sub_type = 0x01;
    out->gamepad.buttons = 0xF3FF;
    out->gamepad.left_trigger = 0xFF;
    out->gamepad.right_trigger = 0xFF;
    out->gamepad.thumb_lx = (int16_t)0xFFC0;
    out->gamepad.thumb_ly = (int16_t)0xFFC0;
    out->gamepad.thumb_rx = (int16_t)0xFFC0;
    out->gamepad.thumb_ry = (int16_t)0xFFC0;
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(rex::input::DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
    return id == kId ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  // Edge events for the title screen / menus, which read XInputGetKeystroke.
  X_RESULT GetDeviceKeystroke(rex::input::DeviceId id, uint32_t,
                              rex::input::X_INPUT_KEYSTROKE* out) override {
    if (id != kId) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (!out) return X_ERROR_BAD_ARGUMENTS;
    rex::input::X_INPUT_STATE st;
    Current(&st);
    uint32_t cur = static_cast<uint16_t>(st.gamepad.buttons);
    if (st.gamepad.left_trigger) cur |= 1u << 16;
    if (st.gamepad.right_trigger) cur |= 1u << 17;
    uint32_t changed = cur ^ last_keys_;
    if (!changed) return X_ERROR_EMPTY;
    // xinput order: releases first, then presses
    for (int pass = 0; pass < 2; pass++) {
      for (int i = 0; i < 18; i++) {
        uint32_t bit = 1u << i;
        if (!(changed & bit) || !kVk[i]) continue;
        bool down = cur & bit;
        if (down != (pass == 1)) continue;
        std::memset(out, 0, sizeof(*out));
        out->virtual_key = kVk[i];
        out->flags = down ? rex::input::X_INPUT_KEYSTROKE_KEYDOWN : rex::input::X_INPUT_KEYSTROKE_KEYUP;
        last_keys_ = down ? (last_keys_ | bit) : (last_keys_ & ~bit);
        return X_ERROR_SUCCESS;
      }
    }
    last_keys_ = cur;
    return X_ERROR_EMPTY;
  }

 private:
  static constexpr rex::input::DeviceId kId = static_cast<rex::input::DeviceId>(0x5C21F7);

  struct Event {
    double t = 0, hold = 0.12;
    uint16_t buttons = 0;
    bool lt = false, rt = false, has_lx = false, has_ly = false;
    int16_t lx = 0, ly = 0;
  };

  // The game polls the pad thousands of times a second: look at the file 4 times a second.
  std::chrono::steady_clock::time_point next_check_{};

  void Reload() {
    auto now = std::chrono::steady_clock::now();
    if (now < next_check_) return;
    next_check_ = now + std::chrono::milliseconds(250);
    std::error_code ec;
    auto size = std::filesystem::file_size(path_, ec);
    if (ec || size == loaded_size_) return;
    std::ifstream f(path_);
    std::vector<Event> events;
    std::string line;
    while (std::getline(f, line)) {
      if (line.empty() || line[0] == '#') continue;
      std::istringstream ss(line);
      Event e;
      std::string keys;
      double hold_ms = 120;
      if (!(ss >> e.t >> keys)) continue;
      ss >> hold_ms;
      e.hold = hold_ms / 1000.0;
      std::istringstream ks(keys);
      std::string k;
      while (std::getline(ks, k, '+')) Apply(e, k);
      events.push_back(e);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    events_ = std::move(events);
    loaded_size_ = size;
  }

  static void Apply(Event& e, const std::string& k) {
    using namespace rex::input;
    if (k.rfind("LX=", 0) == 0 || k.rfind("LY=", 0) == 0) {
      double v = std::clamp(std::atof(k.c_str() + 3), -1.0, 1.0);
      int16_t s = (int16_t)(v * 32767);
      if (k[1] == 'X') { e.lx = s; e.has_lx = true; } else { e.ly = s; e.has_ly = true; }
      return;
    }
    static const struct { const char* name; uint16_t bit; } kMap[] = {
        {"A", X_INPUT_GAMEPAD_A}, {"B", X_INPUT_GAMEPAD_B}, {"X", X_INPUT_GAMEPAD_X},
        {"Y", X_INPUT_GAMEPAD_Y}, {"START", X_INPUT_GAMEPAD_START},
        {"BACK", X_INPUT_GAMEPAD_BACK}, {"UP", X_INPUT_GAMEPAD_DPAD_UP},
        {"DOWN", X_INPUT_GAMEPAD_DPAD_DOWN}, {"LEFT", X_INPUT_GAMEPAD_DPAD_LEFT},
        {"RIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT}, {"LB", X_INPUT_GAMEPAD_LEFT_SHOULDER},
        {"RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER}, {"LS", X_INPUT_GAMEPAD_LEFT_THUMB},
        {"RS", X_INPUT_GAMEPAD_RIGHT_THUMB}};
    if (k == "LT") { e.lt = true; return; }
    if (k == "RT") { e.rt = true; return; }
    for (const auto& m : kMap)
      if (k == m.name) e.buttons |= m.bit;
  }

  // VK_PAD_* by bit of X_INPUT_GAMEPAD::buttons, then LT/RT
  static constexpr uint16_t kVk[18] = {0x5810, 0x5811, 0x5812, 0x5813, 0x5814, 0x5815,
                                       0x5816, 0x5817, 0x5805, 0x5804, 0,      0,
                                       0x5800, 0x5801, 0x5802, 0x5803, 0x5806, 0x5807};
  uint32_t last_keys_ = 0;
  std::string path_;
  std::chrono::steady_clock::time_point start_;
  std::mutex mutex_;
  std::vector<Event> events_;
  uintmax_t loaded_size_ = UINTMAX_MAX;
  uint32_t packet_ = 0;
};
