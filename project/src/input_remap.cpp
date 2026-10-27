#include "platform/native_host_window.h"
// bf2mc - modern controller layout (like recent Battlefield games), applied to the pad state the
// game reads through its XInputGetState wrapper (sub_8246FE70: r3 user index, r4 XINPUT_STATE*).
//
// Original BF2:MC on-foot layout (verified in game / from the game's help texts):
//   RT fire, LT lower stance (crouch / prone), LB raise stance / jump, RB (hold) + right stick
//   weapon select, R3 scope zoom (toggle), A use / enter vehicle, B reload, X hotswap,
//   Y change seat / call troops, L3 sprint, D-pad team radio.
// Modern layout, after Battlefield 6's default (physical button -> original buttons):
//   LT (hold) aim  -> R3 taps until the weapon's zoom index matches LT
//   RT fire -> RT          A jump -> LB (crouched / prone: stands up one step)
//   B tap crouch / stand, B hold prone -> LT / LB taps from the tracked stance
//   X tap reload -> B      X hold use / enter vehicle -> A
//   Y use / vehicle -> A   LB hotswap -> X     RB weapon select -> RB    R3 -> Y
//   L3, D-pad, Start, Back unchanged.
// Keyboard extras (keys without a pad button): Shift sprint (L3), Ctrl (hold) crouch, Z prone.
// Menus keep the original layout. The remap is only active in a mission while it isn't paused:
// the current screen manager (*0x8285EF78) has no screen node (+4216) - frontend menus always have
// one - and the game isn't paused (sub_825D3858 opens the pause menu and sets game+1176,
// sub_825D3BD0 resumes; the pause screen node stays set after resuming, so it is remembered).
//
// --bf2_modern_controls=false restores the original layout everywhere. BF2_INPUT_LOG=1 logs
// screen changes (hashes) and remap activity.
//
// Mouse look (--bf2_mouse_look, on by default with --mnk_mode): the mouse turns the camera
// directly instead of going through the right stick (the SDK's mnk_mouse adds mouse motion to
// the stick, so it inherits the game's response polynomial, acceleration and turn-rate cap).
// sub_826510A8 (r3 look state, r4 look input) is the soldier/turret look update:
//   state: +0 config (+4..+0x20 stick polynomial, +0x30/+0x38 use yaw/pitch acceleration,
//          +0x34 yawSpeed, +0x3C pitchSpeed), +4 yaw (wrapped to 0..2pi), +8 pitch (clamped to
//          input +0x14/+0x18), +0xC/+0x10 acceleration state.
//   input: +0 dt, +0xC/+0x10 stick x/y, +0x1C/+0x20 look multipliers, +0x70 player controlled
//          (0 = turning toward the target at +0x74/+0x78, i.e. AI / scripted aiming).
// The hook adds the mouse motion to yaw/pitch before the update runs, so the game still wraps
// yaw, clamps pitch and builds the view from it. While mouse look is in charge the SDK's own
// mouse look is switched off and the pointer is captured here, only during gameplay.

#include "generated/default/bf2mc_init.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native_gpu/native_gpu.h"
#include "pc_settings.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

REXCVAR_DECLARE(bool, mnk_mode);

REXCVAR_DEFINE_BOOL(bf2_modern_controls, true, "Input",
                    "Modern Battlefield controller layout in game (LT aim, A jump, B crouch, X reload)");
REXCVAR_DEFINE_BOOL(bf2_mouse_look, true, "Input",
                    "Mouse turns the camera directly in game (needs mnk_mode)");
REXCVAR_DEFINE_DOUBLE(bf2_mouse_sensitivity, 1.0, "Input", "Mouse look sensitivity")
    .range(0.05, 20.0);
REXCVAR_DEFINE_BOOL(bf2_mouse_invert, false, "Input", "Invert mouse pitch");

namespace {

constexpr uint32_t kScreenManager = 0x8285EF78;
constexpr uint32_t kManagerNode = 4216;

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_paused{false};
// Optional test aid: retain short default-menu key taps until the guest polls.
// No desktop input is synthesized here; regular window events feed the latch.
std::atomic<bool> g_test_menu{true};
std::atomic<uint16_t> g_test_menu_taps{0};
bool TestMenuTaps() {
  static const bool on=[] {const char* v=std::getenv("BF2_TEST_MENU_TAPS"); return v && *v=='1';}();
  return on;
}

// The local player's soldier controller (sub_821E1748 r3: +0x20 look state, +0x40 zoom index of
// the weapon's zoom list) and look state, refreshed by the look hooks every frame.
uint32_t g_cur_ctrl = 0;  // game thread, during sub_821E1748
std::atomic<uint32_t> g_player_ctrl{0}, g_player_look{0};
std::atomic<int64_t> g_player_seen{0};  // steady_clock ticks of the last player look update
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

// Original-layout buttons the modern layout sends as taps (pressed for 2 polls, released for 2,
// so the game sees each one even if it samples every other call). LT is the original "lower
// stance" trigger; it goes through the same queue as a pseudo button.
constexpr uint32_t kTapLT = 0x10000;

struct PadState {
  uint32_t queue[8] = {};
  int queued = 0, phase = 0;
  void Tap(uint32_t b, int n = 1) {
    while (n-- > 0 && queued < 8) queue[queued++] = b;
  }
  uint32_t Next() {
    if (!queued) return 0;
    uint32_t b = queue[0];
    if (++phase >= 4) {
      phase = 0;
      std::memmove(queue, queue + 1, sizeof(queue[0]) * --queued);
    }
    return phase == 0 || phase >= 3 ? 0 : b;
  }
  bool Busy() const { return queued != 0; }

  // Aim: R3 is a zoom toggle; the wanted state is LT held.
  bool aim_down = false;
  Clock::time_point zoom_tap;  // last R3 sent (the zoom index lags behind it)
  // Stance as this layout last set it (0 standing, 1 crouched, 2 prone); reset on respawn.
  int stance = 0;
  bool hold_crouch = false;    // Ctrl held
  // B / X tap-or-hold.
  Clock::time_point b_down, x_down;
  bool b_held = false, x_held = false, b_long = false, x_long = false;
  bool z_down = false, a_down = false;
  uint32_t look = 0;
};
PadState g_pads[4];

constexpr auto kHoldTime = std::chrono::milliseconds(300);

// Stance changes in the original layout: LT lowers one step, LB raises one step.
void SetStance(PadState& p, int want) {
  if (want > p.stance) p.Tap(kTapLT, want - p.stance);
  else if (want < p.stance) p.Tap(kLB, p.stance - want);
  p.stance = want;
}

// ---------------------------------------------------------------------------- mouse look

// Radians per mouse count at sensitivity 1 and the game's default look multipliers.
constexpr double kRadPerCount = 0.0011;

rex::ui::Window* g_window = nullptr;
HWND g_hwnd = nullptr;
std::mutex g_mouse_mu;
double g_mouse_dx = 0, g_mouse_dy = 0;   // counts not yet applied (g_mouse_mu)
bool g_relative = false;                 // UI thread: pointer locked (dx/dy valid)
int32_t g_prev_x = 0, g_prev_y = 0;      // UI thread: without a pointer lock
std::atomic<bool> g_captured{false};     // pointer held by the mouse look
std::atomic<bool> g_mouse_to_stick{false};  // RB held: the mouse picks a weapon (right stick)
double g_pick_x = 0, g_pick_y = 0;       // game thread: weapon-pick direction
bool g_want_capture = false;             // game thread: last request
Clock::time_point g_last_poll;           // game thread

// Zoom index of the player's weapon (0 = not zoomed), or -1 when the soldier controller isn't
// running (vehicles, menus).
int PlayerZoomIndex(uint8_t* base) {
  uint32_t ctrl = g_player_ctrl.load(std::memory_order_relaxed);
  int64_t seen = g_player_seen.load(std::memory_order_relaxed);
  if (!ctrl || Clock::now().time_since_epoch().count() - seen >
                   std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(300)).count())
    return -1;
  uint32_t index = Load32(base, ctrl + 0x40);
  return index < 16 ? int(index) : -1;
}

bool MouseLookOn() { return REXCVAR_GET(bf2_mouse_look) && REXCVAR_GET(mnk_mode); }

class MouseListener final : public rex::ui::WindowInputListener {
 public:
  // F12: frame capture for renderer debugging (native_gpu: captures/<time>/ next to the exe).
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (TestMenuTaps() && g_test_menu && !e.prev_state() && !pcs::MenuOpen() &&
        REXCVAR_GET(mnk_mode) && !e.is_alt_pressed() && !e.is_ctrl_pressed()) {
      uint16_t tap=0;
      switch(int(e.virtual_key())) {
        case VK_RETURN: tap=kStart; break;
        case VK_SPACE: tap=kA; break;
        case VK_BACK: tap=kB; break;
        case VK_UP: tap=kDpadUp; break;
        case VK_DOWN: tap=kDpadDown; break;
        case VK_LEFT: tap=kDpadLeft; break;
        case VK_RIGHT: tap=kDpadRight; break;
      }
      g_test_menu_taps.fetch_or(tap,std::memory_order_relaxed);
    }
    if (int(e.virtual_key()) == VK_F12 && !pcs::MenuOpen()) {
      if (!e.prev_state()) bf2n::RendererRequestCapture();
      e.set_handled(true);
    }
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    if (!g_captured.load(std::memory_order_relaxed)) {
      g_prev_x = e.x();
      g_prev_y = e.y();
      return;
    }
    double dx, dy;
    if (g_relative) {
      dx = e.dx();
      dy = e.dy();
    } else {
      dx = e.x() - g_prev_x;
      dy = e.y() - g_prev_y;
      g_prev_x = e.x();
      g_prev_y = e.y();
      int32_t w = int32_t(g_window->GetActualPhysicalWidth());
      int32_t h = int32_t(g_window->GetActualPhysicalHeight());
      int32_t cx, cy;
      if (w > 0 && h > 0 &&
          (std::abs(e.x() - w / 2) >= w / 4 || std::abs(e.y() - h / 2) >= h / 4) &&
          g_window->WarpMouseToCenter(cx, cy)) {
        g_prev_x = cx;
        g_prev_y = cy;
      }
    }
    std::lock_guard<std::mutex> lock(g_mouse_mu);
    g_mouse_dx += dx;
    g_mouse_dy += dy;
  }
};
MouseListener g_mouse_listener;

void ApplyCapture(bool capture) {
  // UI thread.
  if (!g_window) return;
  if (capture) {
    g_window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    g_window->CaptureMouse();
    g_relative = g_window->SetRelativeMouseMode(true);
    {
      std::lock_guard<std::mutex> lock(g_mouse_mu);
      g_mouse_dx = g_mouse_dy = 0;
    }
    g_captured = true;
  } else {
    g_captured = false;
    g_window->SetRelativeMouseMode(false);
    g_relative = false;
    g_window->ReleaseMouse();
    g_window->SetCursorVisibility(pcs::Get().display ? rex::ui::Window::CursorVisibility::kAutoHidden
                                                     : rex::ui::Window::CursorVisibility::kVisible);
  }
  if (LogEnabled()) REXLOG_INFO("bf2mc input: mouse {} (pointer lock {})", capture ? "captured" : "released",
                                g_relative);
}

// Game thread, every pad poll: attaches to the window and holds the pointer during gameplay.
void MouseUpdate(bool gameplay) {
  if (!g_window) {
    auto* runtime = rex::Runtime::instance();
    if (!runtime || !runtime->display_window()) return;
    g_window = runtime->display_window();
    g_hwnd = static_cast<HWND>(native_host::WindowHandle(g_window));
    g_window->app_context().CallInUIThreadDeferred(
        [] { g_window->AddInputListener(&g_mouse_listener, 10); });
  }
  if (!MouseLookOn()) {
    if (g_want_capture) {
      g_want_capture = false;
      g_window->app_context().CallInUIThreadDeferred([] { ApplyCapture(false); });
    }
    return;
  }
  // The SDK's mouse look would add the same motion to the right stick.
  if (rex::input::mnk::IsMouseLookActive()) rex::input::mnk::SetMouseLookActive(false);

  bool want = gameplay && !pcs::MenuOpen() && GetForegroundWindow() == g_hwnd;
  // A long gap between polls: the settings menu held the game (it frees the pointer itself) or
  // a load stalled it. Take the pointer again.
  Clock::time_point now = Clock::now();
  bool stalled = now - g_last_poll > std::chrono::milliseconds(250);
  g_last_poll = now;
  if (want != g_want_capture || (want && stalled)) {
    g_want_capture = want;
    g_window->app_context().CallInUIThreadDeferred([want] { ApplyCapture(want); });
  }
}

void TakeMouse(double* dx, double* dy) {
  std::lock_guard<std::mutex> lock(g_mouse_mu);
  *dx = g_mouse_dx;
  *dy = g_mouse_dy;
  g_mouse_dx = g_mouse_dy = 0;
}

inline float LoadF(uint8_t* base, uint32_t a) {
  uint32_t v = Load32(base, a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline void StoreF(uint8_t* base, uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, a), v);
}

}  // namespace

REX_HOOK_RAW(sub_8246FE70) {
  uint32_t user = ctx.r3.u32, state = ctx.r4.u32;
  __imp__sub_8246FE70(ctx, base);
  if (ctx.r3.u32 != 0 || user >= 4 || !state) return;

  bool in_game = InGameplay(base);
  if (user==0 && TestMenuTaps()) {
    g_test_menu=!in_game;
    static uint16_t held=0;
    static Clock::time_point until;
    uint16_t tapped=g_test_menu_taps.exchange(0,std::memory_order_relaxed);
    auto now=Clock::now();
    if(in_game) held=0;
    else {
      if(now>=until) held=0;
      if(tapped) {held|=tapped; until=now+std::chrono::milliseconds(120);}
      uint8_t* s=rex::memory::GuestPtr(base,state);
      rex::memory::store_and_swap<uint16_t>(s+4,rex::memory::load_and_swap<uint16_t>(s+4)|held);
    }
  }
  pcs::SetGameMenus(!in_game);  // the "PC settings" button over the game's menus
  MouseUpdate(in_game);
  bool gameplay = REXCVAR_GET(bf2_modern_controls) && in_game;
  if (LogEnabled()) {
    static std::atomic<int> last{-1};
    if (last.exchange(gameplay) != int(gameplay))
      REXLOG_INFO("bf2mc input: {} layout (screen node {:08X})", gameplay ? "modern" : "original",
                  CurrentScreenNode(base));
  }

  // RB (hold) + right stick is the weapon selector: there the mouse drives the stick.
  if (user == 0 && in_game && g_captured.load(std::memory_order_relaxed)) {
    uint8_t* s = rex::memory::GuestPtr(base, state);
    bool pick = rex::memory::load_and_swap<uint16_t>(s + 4) & kRB;
    g_mouse_to_stick = pick;
    if (pick) {
      double dx, dy;
      TakeMouse(&dx, &dy);
      g_pick_x = std::clamp(g_pick_x + dx, -200.0, 200.0);
      g_pick_y = std::clamp(g_pick_y - dy, -200.0, 200.0);
      double len = std::sqrt(g_pick_x * g_pick_x + g_pick_y * g_pick_y);
      if (len > 40) {
        rex::memory::store_and_swap<int16_t>(s + 12, int16_t(g_pick_x / len * 32767));
        rex::memory::store_and_swap<int16_t>(s + 14, int16_t(g_pick_y / len * 32767));
      }
    } else {
      g_pick_x = g_pick_y = 0;
    }
  }

  PadState& pad = g_pads[user];
  if (!gameplay) {
    pad = PadState{};
    return;
  }

  uint8_t* s = rex::memory::GuestPtr(base, state);
  uint16_t in = rex::memory::load_and_swap<uint16_t>(s + 4);
  uint8_t lt = s[6], rt = s[7];

  // Keyboard extras for the modern layout (keys the pad has no button for): Shift sprint,
  // Ctrl (hold) crouch, Z prone. C is bound to B (crouch toggle) by the launcher.
  bool kb = user == 0 && REXCVAR_GET(mnk_mode) && g_hwnd && GetForegroundWindow() == g_hwnd;
  auto key = [kb](int vk) { return kb && (GetAsyncKeyState(vk) & 0x8000) != 0; };

  Clock::time_point now = Clock::now();
  uint16_t out = in & (kDpadUp | kDpadDown | kDpadLeft | kDpadRight | kStart | kBack | kL3 | kRB);
  if (key(VK_SHIFT)) out |= kL3;
  if (in & kY) out |= kA;
  if (in & kLB) out |= kX;
  if (in & kR3) out |= kY;

  // Respawn (a new look state) stands the soldier up.
  uint32_t look = g_player_look.load(std::memory_order_relaxed);
  if (look != pad.look) {
    pad.look = look;
    pad.stance = 0;
  }

  // A: jump; crouched or prone it stands up one step first, like the original LB.
  if ((in & kA) && !pad.a_down) {
    if (pad.stance > 0) SetStance(pad, pad.stance - 1);
    else pad.Tap(kLB);
  }
  pad.a_down = in & kA;

  // B: tap toggles crouch, hold goes prone (from prone, a tap stands up).
  bool b = in & kB;
  if (b && !pad.b_held) { pad.b_down = now; pad.b_long = false; }
  if (b && !pad.b_long && now - pad.b_down >= kHoldTime) {
    pad.b_long = true;
    SetStance(pad, 2);
  }
  if (!b && pad.b_held && !pad.b_long) SetStance(pad, pad.stance == 0 ? 1 : 0);
  pad.b_held = b;

  // X: tap reloads, hold uses / enters a vehicle (held as long as X is).
  bool x = in & kX;
  if (x && !pad.x_held) { pad.x_down = now; pad.x_long = false; }
  if (x && now - pad.x_down >= kHoldTime) pad.x_long = true;
  if (pad.x_long && x) out |= kA;
  if (!x && pad.x_held && !pad.x_long) pad.Tap(kB);
  pad.x_held = x;

  // Ctrl held: crouched while held. Z: prone toggle.
  bool ctrl = key(VK_CONTROL);
  if (ctrl != pad.hold_crouch) {
    pad.hold_crouch = ctrl;
    SetStance(pad, ctrl ? 1 : 0);
  }
  bool z = key('Z');
  if (z && !pad.z_down) SetStance(pad, pad.stance == 2 ? 0 : 2);
  pad.z_down = z;

  // Aim: tap R3 until the weapon's zoom index matches LT (an R3 during the zoom-in animation is
  // ignored, so edge taps alone could end up zoomed with LT released). Without a known zoom
  // index (vehicles), tap on each LT edge.
  bool aim = lt >= kTriggerDown;
  int zoom = PlayerZoomIndex(base);
  if (zoom >= 0) {
    if ((zoom != 0) != aim && !pad.Busy() && now - pad.zoom_tap > std::chrono::milliseconds(350)) {
      pad.Tap(kR3);
      pad.zoom_tap = now;
    }
  } else if (aim != pad.aim_down) {
    pad.Tap(kR3);
  }
  if (aim != pad.aim_down && LogEnabled())
    REXLOG_INFO("bf2mc input: aim {} (zoom index {})", aim ? "on" : "off", zoom);
  pad.aim_down = aim;

  uint32_t tap = pad.Next();
  out |= uint16_t(tap);
  rex::memory::store_and_swap<uint16_t>(s + 4, out);
  s[6] = (tap & kTapLT) ? 255 : 0;
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

// Look update (r3 look state, r4 look input): mouse motion goes straight into yaw / pitch.
REX_HOOK_RAW(sub_826510A8) {
  uint32_t st = ctx.r3.u32, in = ctx.r4.u32;
  if (st && in && g_cur_ctrl && Load32(base, in + 0x70) >> 24) {
    g_player_ctrl = g_cur_ctrl;
    g_player_look = st;
    g_player_seen = Clock::now().time_since_epoch().count();
  }
  if (st && in && g_captured.load(std::memory_order_relaxed) && !g_mouse_to_stick &&
      Load32(base, in + 0x70) >> 24) {
    double dx, dy;
    TakeMouse(&dx, &dy);
    // The game's own look multipliers (aim sensitivity, zoom) scale the mouse too.
    float m1 = LoadF(base, in + 0x1C), m2 = LoadF(base, in + 0x20);
    double scale = kRadPerCount * REXCVAR_GET(bf2_mouse_sensitivity) * m1 * m2;
    if (LogEnabled()) {
      static Clock::time_point next;
      if (Clock::now() >= next) {
        next = Clock::now() + std::chrono::seconds(1);
        REXLOG_INFO("bf2mc look: state {:08X} dt {:.4f} stick {:.2f},{:.2f} mult {:.3f},{:.3f} "
                    "yaw {:.3f} pitch {:.3f} limits {:.3f},{:.3f} mouse {:.0f},{:.0f}",
                    st, LoadF(base, in), LoadF(base, in + 0xC), LoadF(base, in + 0x10), m1, m2,
                    LoadF(base, st + 4), LoadF(base, st + 8), LoadF(base, in + 0x14),
                    LoadF(base, in + 0x18), dx, dy);
      }
    }
    if ((dx || dy) && std::isfinite(scale)) {
      constexpr float kTwoPi = 6.28318530718f;
      float yaw = LoadF(base, st + 4) + float(dx * scale);
      yaw = std::fmod(yaw, kTwoPi);
      if (yaw < 0) yaw += kTwoPi;
      float pitch = LoadF(base, st + 8) +
                    float((REXCVAR_GET(bf2_mouse_invert) ? dy : -dy) * scale);
      StoreF(base, st + 4, yaw);
      StoreF(base, st + 8, pitch);
    }
  }
  __imp__sub_826510A8(ctx, base);
}

// Soldier controller update (r3 controller); it runs the look update above.
REX_HOOK_RAW(sub_821E1748) {
  uint32_t prev = g_cur_ctrl;
  g_cur_ctrl = ctx.r3.u32;
  __imp__sub_821E1748(ctx, base);
  g_cur_ctrl = prev;
}
