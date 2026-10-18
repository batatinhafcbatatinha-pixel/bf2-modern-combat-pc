// bf2mc - modern controller layout (like recent Battlefield games), applied to the pad state the
// game reads through its XInputGetState wrapper (sub_8246FE70: r3 user index, r4 XINPUT_STATE*).
//
// Original BF2:MC on-foot layout (verified in game / from the game's help texts):
//   RT fire, LT lower stance (crouch / prone), LB raise stance / jump, RB (hold) + right stick
//   weapon select, R3 scope zoom (toggle), A use / enter vehicle, B reload, X hotswap,
//   Y change seat / call troops, D-pad team radio.
// Modern layout (physical button -> original button):
//   LT (hold) aim  -> R3 tap on press and on release      RT fire          -> RT
//   A jump         -> LB                                   B crouch / prone -> LT
//   X reload       -> B                                    Y use / vehicle  -> A
//   LB hotswap     -> X                                    RB weapon select -> RB
//   R3             -> Y (change seat / call troops)        L3, D-pad, Start, Back unchanged
// Menus keep the original layout. The remap is only active in a mission while it isn't paused:
// the current screen manager (*0x8285EF78) has no screen node (+4216) - frontend menus always have
// one - and the game isn't paused (sub_825D3858 opens the pause menu and sets game+1176,
// sub_825D3BD0 resumes; the pause screen node stays set after resuming, so it is remembered).
//
// --bf2_modern_controls=false restores the original layout everywhere. BF2_INPUT_LOG=1 logs
// screen changes (hashes) and remap activity.

#include "generated/default/bf2mc_init.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

REXCVAR_DEFINE_BOOL(bf2_modern_controls, true, "Input",
                    "Modern Battlefield controller layout in game (LT aim, A jump, B crouch, X reload)");

namespace {

constexpr uint32_t kScreenManager = 0x8285EF78;
constexpr uint32_t kManagerNode = 4216;

std::atomic<bool> g_paused{false};
std::atomic<uint32_t> g_pause_node{0};

enum : uint16_t {
  kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008,
  kStart = 0x0010, kBack = 0x0020, kL3 = 0x0040, kR3 = 0x0080,
  kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000,
};
constexpr uint8_t kTriggerDown = 64;

inline uint32_t Load32(uint8_t* base, uint32_t a) {
  return rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, a));
}

bool LogEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("BF2_INPUT_LOG");
    return v && *v && *v != '0';
  }();
  return on;
}

// Screen node of the current manager; UINT32_MAX without a manager (boot, level loads).
uint32_t CurrentScreenNode(uint8_t* base) {
  uint32_t manager = Load32(base, kScreenManager);
  return manager ? Load32(base, manager + kManagerNode) : UINT32_MAX;
}

bool InGameplay(uint8_t* base) {
  if (g_paused) return false;
  uint32_t node = CurrentScreenNode(base);
  return node == 0 || (node != UINT32_MAX && node == g_pause_node);
}

// Per pad: aim (LT) edge tracking and the R3 taps it generates (pressed for 2 polls, released
// for 2 polls, so the game sees each tap even if it samples every other call).
struct PadState {
  bool aim_down = false;
  int taps = 0;
  int phase = 0;
};
PadState g_pads[4];

uint16_t NextTap(PadState& p) {
  if (!p.taps) return 0;
  int step = p.phase++;
  if (step >= 3) {
    p.phase = 0;
    p.taps--;
  }
  return step < 2 ? kR3 : 0;
}

}  // namespace

REX_HOOK_RAW(sub_8246FE70) {
  uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_8246FE70(ctx, base);
  if (ctx.r3.u32 != 0 || user >= 4 || !state) return;

  bool gameplay = REXCVAR_GET(bf2_modern_controls) && InGameplay(base);
  if (LogEnabled()) {
    static std::atomic<int> last{-1};
    if (last.exchange(gameplay) != int(gameplay))
      REXLOG_INFO("bf2mc input: {} layout (screen node {:08X})", gameplay ? "modern" : "original",
                  CurrentScreenNode(base));
  }

  PadState& pad = g_pads[user];
  if (!gameplay) {
    pad = PadState{};
    return;
  }

  uint8_t* s = rex::memory::GuestPtr(base, state);
  uint16_t in = rex::memory::load_and_swap<uint16_t>(s + 4);
  uint8_t lt = s[6], rt = s[7];

  uint16_t out = in & (kDpadUp | kDpadDown | kDpadLeft | kDpadRight | kStart | kBack | kL3 | kRB);
  if (in & kA) out |= kLB;
  if (in & kX) out |= kB;
  if (in & kY) out |= kA;
  if (in & kLB) out |= kX;
  if (in & kR3) out |= kY;
  uint8_t out_lt = (in & kB) ? 255 : 0;

  bool aim = lt >= kTriggerDown;
  if (aim != pad.aim_down) {
    pad.aim_down = aim;
    pad.taps++;
    if (LogEnabled()) REXLOG_INFO("bf2mc input: aim {}", aim ? "on" : "off");
  }
  out |= NextTap(pad);

  rex::memory::store_and_swap<uint16_t>(s + 4, out);
  s[6] = out_lt;
  s[7] = rt;
}

// Pause / resume of the mission (r3 = game object).
REX_HOOK_RAW(sub_825D3858) {
  g_paused = true;
  __imp__sub_825D3858(ctx, base);
  g_pause_node = CurrentScreenNode(base);
}

REX_HOOK_RAW(sub_825D3BD0) {
  __imp__sub_825D3BD0(ctx, base);
  g_paused = false;
}
