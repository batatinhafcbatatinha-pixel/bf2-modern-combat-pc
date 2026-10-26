// In-game PC settings for the Xbox 360 ports (same screen and items as the FIFA Street 1 port).
//
// F1 on the keyboard, or Back+Start on a controller, opens a full-window menu drawn over the
// game. While it is open the game thread is held in the renderer's present (the picture stays
// on screen, the sound is muted) and every key, mouse click and pad press goes to the menu.
// The mouse works the menu too: pointing selects a line, left click changes it forward (or
// runs it), right click backward, the wheel steps through the lines.
//
// The module owns the final step of every frame: the renderer hands it the guest front buffer
// (pcs::Picture) and pcs::Present draws it into the swap chain (aspect, FXAA), adds the menu
// and the FPS counter, presents with or without vsync and paces the frame rate. Display mode
// (window / borderless / exclusive fullscreen), monitor, refresh rate, vsync, frame limit,
// FXAA, anisotropic filtering, aspect and volume apply at once; internal resolution, graphics
// card and the game's language are read at boot and the menu offers a restart.
//
// Settings live in <game>-pc.ini next to the executable ([opcoes]). PCS_OPTIONS=0 ignores the
// ini (test runs); PCS_FPS_LOG=1 logs the frame rate every 5 s.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;
struct ID3D11RenderTargetView;
struct IDXGISwapChain1;
struct IDXGIAdapter1;

namespace pcs {

constexpr int kFpsMonitor = -1;

struct Settings {
  int display = 0;      // 0 window, 1 borderless fullscreen, 2 exclusive fullscreen
  int monitor = 0;
  int refresh = 0;      // Hz in exclusive fullscreen; 0 = the monitor's current rate
  int vsync = 1;
  int fps = 60;         // frame rate: 60, 30, kFpsMonitor (the display's rate) or 0 (no limit)
  int scale = 0;        // internal resolution preset (restart)
  int aspect = 0;       // 0 original 16:9 with bars, 1 stretched to the window
  int aa = 0;           // FXAA
  int aniso = 0;        // 0 = the game's filtering, else 2..16
  int adapter = -1;     // DXGI adapter index; -1 = Windows default (restart)
  int api = 0;          // 0 Direct3D 11, 1 Vulkan through DXVK (vulkan\ next to the exe) (restart)
  int ui_language = 0;  // this menu: 0 Portuguese, 1 English
  int language = 0;     // game language (XC_LANGUAGE id; 0 = the console default) (restart)
  int volume = 100;
  int show_fps = 0;
};

struct Language {
  int id;               // XC_LANGUAGE
  const wchar_t* name;
};

// Optional runtime-owned menu assets. Pixels are straight-alpha 0xAARRGGBB;
// colours below are 0xRRGGBB. Metrics use a 1280x720 design canvas.
struct ThemeGlyph {
  uint16_t code, x, y;
  uint8_t width, height;
  int8_t offset_x;
  uint8_t offset_y;
  uint16_t advance;
};
struct ThemeImage {
  int width = 0, height = 0;
  std::vector<uint32_t> rgba;
};
struct ThemeFont {
  ThemeImage atlas;
  std::vector<ThemeGlyph> glyphs;
  int line_height = 22;
};
struct Theme {
  ThemeFont font;
  ThemeFont title_font;  // optional larger face for the title (empty = font)
  ThemeImage highlight, button_a, button_b, panel_top, panel_bottom;
  uint32_t text = 0xe5e5cf, muted = 0x929582, accent = 0xb8ba56;
  uint32_t selected_text = 0x171a10, panel = 0x20251b;
  // Optional per-part colours (0 = the colour above).
  uint32_t row_text = 0, title_text = 0, title_outline = 0;
  int row_height = 21, padding = 12, panel_width = 960, top = 160;
  int row_gap = 0;  // gap between row bars (the selected bar keeps the gap too)
  bool title_left = false;
  bool title_rule = false;        // accent rule under the title
  bool flat_highlight = false;    // solid accent bar instead of the stripe fallback
  bool dots = true;               // dotted frame accents of the shared design
  float text_scale = 1.0f;        // on top of the 1280x720 canvas scale
  float title_scale = 1.0f;       // on top of the shared 1.25x title scale
  void (*load)(Theme&) = nullptr;
};

// A post-process effect the game draws that the player may turn off.
struct Effect {
  const char* key;          // ini key in [opcoes], e.g. "motion_blur"
  const wchar_t* name_pt;
  const wchar_t* name_en;
  const wchar_t* help_pt;
  const wchar_t* help_en;
  bool default_on = true;
};

struct GameInfo {
  const wchar_t* title = L"";        // e.g. L"FIFA STREET 3"
  const char* window_title = "";     // e.g. "FIFA Street 3"
  const wchar_t* ini_name = L"";     // e.g. L"fifastreet3-pc.ini"
  const Language* languages = nullptr;
  int language_count = 0;
  bool internal_resolution = false;  // the renderer honours pcs::ResolutionScale()
  // The game measures real time (its simulation follows the clock, not the number of frames):
  // it may run above 60 fps, so the menu offers "monitor rate" and "no limit". Games that
  // advance one fixed step per frame keep 60 (more frames would speed them up).
  bool real_time = false;
  // Explicit opt-in to high frame rates before simulation timing has been validated.
  bool unlocked_fps = false;
  int native_width = 1280, native_height = 720;
  // Football games: the keyboard plays as the controller with the EA SPORTS FC 27 "WASD"
  // layout (basic actions only), unless the ini has teclado=0.
  bool football_keyboard = false;
  const Theme* theme = nullptr;  // caller-owned, must outlive the renderer
  const Effect* effects = nullptr;  // caller-owned, must outlive the renderer
  int effect_count = 0;             // at most 8
};

// The guest picture to show this frame.
struct Picture {
  ID3D11ShaderResourceView* srv = nullptr;  // null: nothing to show (black)
  float u1 = 1.0f, v1 = 1.0f;               // used part of the texture (0..u1, 0..v1)
  float width = 1280.0f, height = 720.0f;   // guest picture size (aspect)
  bool swap_rb = false;                     // stored BGRA (8_8_8_8 resolves)
};

// The renderer's back buffer, sized to the window (the renderer resizes its swap chain here,
// passing pcs::SwapChainFlags() to ResizeBuffers). Returns false when there is none.
using BackBufferFn = std::function<bool(ID3D11RenderTargetView** rtv, uint32_t* w, uint32_t* h)>;

// Process start (the app's OnPreSetup, before the window exists): reads the ini and, with
// `sdk_flags`, sets the SDK flags (window size, fullscreen, game language). A game module in
// its own DLL has its own copy of this state: it calls Configure too, without `sdk_flags`.
void Configure(const GameInfo& info, bool sdk_flags = true);
const Settings& Get();

// Boot-time values for the renderer.
float ResolutionScale();          // internal resolution factor (1 = 720p); changes live (read per frame)
// Live, thread-safe effect value. Before Configure supplies descriptors, defaults to on.
// Configure applies each descriptor's default_on; out-of-range indices return on.
bool EffectEnabled(int index);
int Anisotropy();                 // 0 = the game's filtering
uint32_t SwapChainFlags();        // for CreateSwapChainForHwnd and ResizeBuffers
IDXGIAdapter1* Adapter();         // chosen adapter (caller releases) or null for the default
bool Vulkan();                    // the device is DXVK's (Vulkan)
// The renderer's device, on the chosen API and card (feature level 11_1, else 11_0). An HRESULT.
long CreateDevice(unsigned flags, ID3D11Device** device, ID3D11DeviceContext** context);
float Volume();                   // 0..1 (0 while the menu is open)
// Rate of the guest's vertical blank: the vblank interrupt and the flip pacing of the
// presentation interval. 60 as on the console; for a real-time game, the chosen frame rate
// (the monitor's, or 500 with no limit). Changes live: read it every tick. PCS_VBLANK_HZ
// forces a rate (timing experiments).
double GuestRefreshHz();
bool MenuOpen();
// Opens the menu at the next frame (an entry of the game's own menus leads here).
void RequestMenu();
// The game is on its own menus (title, main menu, options, pause): a "PC settings" button is
// shown over them (BACK + START / F1, or a click). Call it every frame or input poll; the button
// hides by itself when the calls stop for half a second. ini botao_menu=0 hides it.
void SetGameMenus(bool on_menus);
// Lines of the game's own menus lead to this menu (ini menu_no_jogo, default on; read by every
// module of the process).
bool GameMenuEntry();

// Once per guest frame, from the thread that draws: shows `picture`, runs the menu while it is
// open (blocking), presents and paces.
void Present(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain1* swap_chain,
             const Picture& picture, const BackBufferFn& back_buffer);

}  // namespace pcs

