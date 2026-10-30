// Post-process effects the player may turn off in PC SETTINGS (pc_game_info.h, kEffects).
//
// Bloom: the Lion post-process manager (singleton returned by sub_8233C5A8) keeps the shader
// parameters of its bloom/tone-map passes in a named table at manager+668 (count u16 at +42,
// 56-byte records at +48: name, then the float value at +40; lookup sub_8234BFC8). The level's
// settings are copied in by sub_82357880: bloomAmount, bloomOffset, sceneAmount and the blur
// widths. The composite adds the blurred bright parts times bloomAmount to the scene, so 0 leaves
// the scene alone. Forced every frame before sub_8233C608 schedules the passes; the game's value
// is put back when the effect is turned on again.
// Not verified in game (static analysis only).
#ifndef _WIN32
#include <windows.h>
#include "platform/native_win32_posix.h"
#endif
#include "generated/default/bf2mc_init.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/memory.h>

#include "pc_game_info.h"

namespace {

uint32_t Load32(uint8_t* base, uint32_t a) { return rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, a)); }
uint16_t Load16(uint8_t* base, uint32_t a) { return rex::memory::load_and_swap<uint16_t>(rex::memory::GuestPtr(base, a)); }
void Store32(uint8_t* base, uint32_t a, uint32_t v) { rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, a), v); }

// Guest address of the value of parameter `name` in the table at `table`, or 0.
uint32_t ParamValue(uint8_t* base, uint32_t table, const char* name) {
  uint32_t count = Load16(base, table + 42), records = Load32(base, table + 48);
  if (!records || count > 256) return 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t rec = records + 56 * i;
    if (_strnicmp(reinterpret_cast<const char*>(rex::memory::GuestPtr(base, rec)), name, 40) == 0) return rec + 40;
  }
  return 0;
}

uint32_t g_bloom_saved = 0;  // the game's bloomAmount while bloom is off (float bits)
bool g_bloom_forced = false;

}  // namespace

// sub_8233C608(manager, ...): draws the bloom / exposure passes of the frame.
REX_HOOK_RAW(sub_8233C608) {
  const uint32_t manager = ctx.r3.u32;
  if (manager) {
    uint32_t amount = ParamValue(base, manager + 668, "bloomAmount");
    if (amount) {
      const bool on = pcs::EffectEnabled(kEffectBloom);
      uint32_t value = Load32(base, amount);
      if (!on) {
        if (value != 0) g_bloom_saved = value;  // first frame off, or the game set a new level value
        g_bloom_forced = true;
        Store32(base, amount, 0);
      } else if (g_bloom_forced) {
        if (value == 0) Store32(base, amount, g_bloom_saved);
        g_bloom_forced = false;
      }
    }
  }
  __imp__sub_8233C608(ctx, base);
}
