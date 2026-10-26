// "PC SETTINGS" line inside the game's own OPTIONS submenu (main menu, both pause menus with
// submenus and the campaign screen), drawn by the game like its other lines.
//
// The menus are EA APT movies (Flash-like ActionScript bytecode; border\menus\apt\*.cat in
// Data1.viv). Each builds its OPTIONS submenu as
//   submenuN = new Array([GENERAL OPTIONS], [DISPLAY OPTIONS], [OTHER OPTIONS])   (pairs [id, text])
// and doSubmenuKey(key) sends the chosen line to C++ through extern.optionSelected.
//
// sub_8215BC70 (r3 action block, r4 the movie's .apt base, r5 its .const base, or 0 on a later
// pass) prepares a block of a movie that was just loaded: operands become absolute pointers and
// the constants of push lists become pointers to value objects (made on the way and shared).
// When it has done that for a block named in kMovies (offset and first bytes checked before the
// call), the block is patched in place: an instruction is replaced by a jump to code written here
// (in guest memory), which runs it and the additions, then jumps back.
//   - build block: a 4th pair ["subMenus", "subMenus"] goes into the array (new Array gets 2 + 2
//     arguments). "subMenus" is a string of that block; GetDatabaseString looks texts up by
//     7-letter ids, so the lookup (sub_822BDF30) is answered here with "PC SETTINGS".
//   - doSubmenuKey's select branch: line 4 plays the select sound, calls the movie's
//     getDatabaseString("popupOption", ...) and returns (the menu stays as it is); that lookup opens
//     the PC settings.
// The values the added code pushes are taken from push lists of the same block that already hold
// them (Ref = push instruction, position). The table comes from the USA disc's movies (scratchpad
// aptgen2.py). bf2mc-pc.ini menu_no_jogo=0 keeps the original menus; BF2_APT_LOG=1 logs the
// patches and the lines chosen.
#include "generated/default/bf2mc_init.h"

#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "pc_settings.h"

namespace {

uint32_t Load32(uint8_t* base, uint32_t a) { return rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, a)); }
void Store32(uint8_t* base, uint32_t a, uint32_t v) { rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, a), v); }
uint32_t Align4(uint32_t a) { return (a + 3) & ~3u; }

bool LogOn() {
  static const bool on = [] { const char* e = std::getenv("BF2_APT_LOG"); return e && *e == '1'; }();
  return on;
}

constexpr char kTextId[] = "subMenus";     // the added line's text id
constexpr char kOpenId[] = "popupOption";  // looked up when the line is chosen

struct Ref {
  uint32_t push;  // offset in .apt of a push instruction
  uint32_t pos;   // position in its list
};

struct MoviePatch {
  const char* name;
  // Block that builds the OPTIONS submenu (offset in .apt, its first 16 bytes before preparation).
  uint32_t build_block;
  uint8_t build_sig[16];
  uint32_t s1, s1_len;  // push [submenuN, "OTHER OPTIONS", "FE00071", 2]
  uint32_t s2, s2_len;  // push [3, "Array"] before `new`
  Ref sub_menus;        // "subMenus"
  // Block holding doSubmenuKey.
  uint32_t key_block;
  uint8_t key_sig[16];
  uint32_t s3, s3_len;  // first instruction of the select-key branch (a push)
  Ref popup, ext, sound, int1, lookup, int100, int2;  // popupOption, extern, selectSound, getDatabaseString
};

const MoviePatch kMovies[] = {
    {"main_menu", 0x6DC8, {0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa7, 0x00, 0x00, 0x8f, 0x20, 0x96, 0x00, 0x00, 0x00}, 0x73EE, 10, 0x7411, 11, {0x741E, 0},
     0x4794, {0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xb4, 0x00, 0x00, 0x79, 0x98, 0x8e, 0x00, 0x00, 0x00}, 0x691C, 12, {0x66A8, 0}, {0x66C8, 0}, {0x6929, 0}, {0x66D5, 1}, {0x49E0, 3}, {0x6789, 1}, {0x68C1, 2}},
    {"pause_menu_dc", 0x24E0, {0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x35, 0x68, 0x96, 0x00, 0x00, 0x00}, 0x2781, 11, 0x27A5, 11, {0x27B2, 0},
     0xF90, {0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x57, 0x00, 0x00, 0x29, 0x5c, 0x8e, 0x00, 0x00, 0x00}, 0x1E30, 12, {0x1B54, 0}, {0x1B74, 0}, {0x1F09, 0}, {0x1B81, 1}, {0x139D, 1}, {0x1C65, 1}, {0x1E71, 0}},
    {"pause_menu_dm", 0x2434, {0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x43, 0x00, 0x00, 0x34, 0xa0, 0x96, 0x00, 0x00, 0x00}, 0x26F5, 11, 0x2719, 11, {0x2726, 0},
     0xF90, {0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56, 0x00, 0x00, 0x28, 0xd0, 0x8e, 0x00, 0x00, 0x00}, 0x1D9C, 12, {0x1AC0, 0}, {0x1AE0, 0}, {0x1E75, 0}, {0x1AED, 1}, {0x139D, 1}, {0x1BD1, 1}, {0x1DDD, 0}},
    {"campaignscreen", 0x5E7C, {0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x00, 0x00, 0x81, 0x94, 0x96, 0x00, 0x00, 0x00}, 0x68EE, 10, 0x6911, 11, {0x691E, 0},
     0x36EC, {0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x95, 0x00, 0x00, 0x6d, 0xf4, 0x8e, 0x00, 0x00, 0x00}, 0x5344, 12, {0x50C0, 0}, {0x5120, 0}, {0x5351, 0}, {0x512D, 1}, {0x3DED, 1}, {0x51E1, 1}, {0x53BD, 0}},
};

uint32_t GuestAlloc(uint32_t size) {
  uint32_t a = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  if (a) std::memset(rex::memory::GuestPtr(REX_KERNEL_MEMORY()->virtual_membase(), a), 0, size);
  return a;
}

// A prepared push instruction: opcode, then (aligned) count and list pointer.
struct Push {
  uint32_t count = 0, list = 0;
  bool ok = false;
};
Push ReadPush(uint8_t* base, uint32_t at) {
  Push p;
  uint32_t operand = Align4(at + 1);
  p.count = Load32(base, operand);
  p.list = Load32(base, operand + 4);
  p.ok = *rex::memory::GuestPtr(base, at) == 0x96 && p.count && p.count <= 64 && p.list;
  return p;
}
// The value a push list holds, 0 if the instruction isn't the expected push.
uint32_t Value(uint8_t* base, uint32_t apt, const Ref& r) {
  Push p = ReadPush(base, apt + r.push);
  return p.ok && r.pos < p.count ? Load32(base, p.list + 4 * r.pos) : 0;
}

// APT bytecode writer: one opcode byte; operands are 4-byte aligned big-endian words.
struct Writer {
  uint8_t* base;
  uint32_t data, data_end;  // push lists
  uint32_t code, code_end;

  uint32_t List(std::initializer_list<uint32_t> values) {
    uint32_t at = data;
    for (uint32_t v : values) Store32(base, data, v), data += 4;
    return at;
  }
  void Op(uint8_t op) { *rex::memory::GuestPtr(base, code++) = op; }
  void Word(uint32_t v) {
    while (code & 3) Op(0);
    Store32(base, code, v);
    code += 4;
  }
  void PushList(uint32_t list, uint32_t count) { Op(0x96); Word(count); Word(list); }
  // Branch (0x99 jump, 0x9D jump if true) to `target`; returns the operand (to fix later).
  uint32_t Branch(uint8_t op, uint32_t target = 0) {
    Op(op);
    Word(0);
    if (target) Fix(code - 4, target);
    return code - 4;
  }
  void Fix(uint32_t operand, uint32_t target) { Store32(base, operand, target - (operand + 4)); }
  bool Ok() const { return data <= data_end && code <= code_end; }
};

bool NewWriter(uint8_t* base, Writer& w) {
  uint32_t mem = GuestAlloc(320);
  if (!mem) return false;
  w = {base, mem, mem + 96, mem + 96, mem + 320};
  return true;
}

// Replaces the instruction at `site` (`len` bytes) with a jump to `target`, the rest with pops
// (never reached; they keep the block walkable).
void JumpTo(uint8_t* base, uint32_t site, uint32_t len, uint32_t target) {
  uint8_t* p = rex::memory::GuestPtr(base, site);
  uint32_t operand = Align4(site + 1);
  p[0] = 0x99;
  for (uint32_t a = site + 1; a < operand; ++a) p[a - site] = 0;
  Store32(base, operand, target - (operand + 4));
  for (uint32_t a = operand + 4; a < site + len; ++a) p[a - site] = 0x17;
}

void PatchBuild(uint8_t* base, uint32_t apt, const MoviePatch& m) {
  const uint32_t site = apt + m.s1, s2 = apt + m.s2;
  const Push first = ReadPush(base, site), count = ReadPush(base, s2);
  const uint32_t id = Value(base, apt, m.sub_menus);
  Writer w;
  if (!first.ok || first.count != 4 || !count.ok || count.count != 2 || !id || !NewWriter(base, w)) {
    REXLOG_WARN("[apt] {}: OPTIONS submenu not as expected, left alone", m.name);
    return;
  }
  const uint32_t submenu = Load32(base, first.list), two = Load32(base, first.list + 12),
                 array = Load32(base, count.list + 4);
  const uint32_t pair = w.List({submenu, id, id, two}), twos = w.List({two, two}), array_name = w.List({array});
  const uint32_t entry = w.code;
  w.PushList(pair, 4);            // submenuN, "subMenus", "subMenus", 2
  w.Op(0x42);                     // initArray -> the added pair
  w.PushList(first.list + 4, 3);  // "OTHER OPTIONS", "FE00071", 2 (the replaced push without submenuN)
  w.Branch(0x99, site + m.s1_len);
  const uint32_t entry2 = w.code;
  w.PushList(twos, 2);            // 2 + 2 instead of 3
  w.Op(0x47);                     // add2
  w.PushList(array_name, 1);      // "Array"
  w.Branch(0x99, s2 + m.s2_len);
  if (!w.Ok()) return;
  JumpTo(base, site, m.s1_len, entry);
  JumpTo(base, s2, m.s2_len, entry2);
  if (LogOn()) REXLOG_INFO("[apt] {}: OPTIONS submenu gets PC SETTINGS (code {:08X})", m.name, entry);
}

void PatchSelect(uint8_t* base, uint32_t apt, const MoviePatch& m) {
  const uint32_t site = apt + m.s3;
  const Push orig = ReadPush(base, site);
  const uint32_t popup = Value(base, apt, m.popup), ext = Value(base, apt, m.ext), sound = Value(base, apt, m.sound),
                 one = Value(base, apt, m.int1), lookup = Value(base, apt, m.lookup),
                 hundred = Value(base, apt, m.int100), two = Value(base, apt, m.int2);
  Writer w;
  if (!orig.ok || !popup || !ext || !sound || !one || !lookup || !hundred || !two || !NewWriter(base, w)) {
    REXLOG_WARN("[apt] {}: doSubmenuKey not as expected, left alone", m.name);
    return;
  }
  const uint32_t l_popup = w.List({popup}), l_four = w.List({two, two}), l_ext = w.List({ext}),
                 l_sound = w.List({sound, one}), l_open = w.List({popup, popup, two, lookup}),
                 l_ret = w.List({hundred});
  const uint32_t entry = w.code;
  // if (popupOption == 2 + 2) { extern.selectSound = 1; getDatabaseString("popupOption", "popupOption"); return 100; }
  w.PushList(l_popup, 1);
  w.Op(0x1C);  // getVar
  w.PushList(l_four, 2);
  w.Op(0x47);  // add2
  w.Op(0x49);  // equals2
  w.Op(0x12);  // not
  const uint32_t to_original = w.Branch(0x9D);
  w.PushList(l_ext, 1);
  w.Op(0x1C);
  w.PushList(l_sound, 2);
  w.Op(0x4F);  // setMember
  w.PushList(l_open, 4);
  w.Op(0x3D);  // callFunction
  w.Op(0x17);  // pop
  w.PushList(l_ret, 1);
  w.Op(0x3E);  // return
  w.Fix(to_original, w.code);
  w.PushList(orig.list, orig.count);  // the replaced instruction
  w.Branch(0x99, site + m.s3_len);
  if (!w.Ok()) return;
  JumpTo(base, site, m.s3_len, entry);
  if (LogOn()) REXLOG_INFO("[apt] {}: doSubmenuKey knows line 4 (code {:08X})", m.name, entry);
}

}  // namespace

REX_HOOK_RAW(sub_8215BC70) {
  const uint32_t block = ctx.r3.u32, apt = ctx.r4.u32, constants = ctx.r5.u32;
  const MoviePatch* build = nullptr;
  const MoviePatch* select = nullptr;
  if (block && apt && constants && block > apt && pcs::GameMenuEntry()) {
    const uint32_t off = block - apt;
    const uint8_t* p = rex::memory::GuestPtr(base, block);
    for (const MoviePatch& m : kMovies) {
      if (off == m.build_block && std::memcmp(p, m.build_sig, 16) == 0) build = &m;
      if (off == m.key_block && std::memcmp(p, m.key_sig, 16) == 0) select = &m;
    }
  }
  __imp__sub_8215BC70(ctx, base);
  if (build) PatchBuild(base, apt, *build);
  if (select) PatchSelect(base, apt, *select);
}

// Text database lookup by id (r3 = 7-letter id; returns the UTF-16BE text or 0).
REX_HOOK_RAW(sub_822BDF30) {
  const uint32_t key = ctx.r3.u32;
  const char* id = key ? reinterpret_cast<const char*>(rex::memory::GuestPtr(base, key)) : "";
  if (std::strncmp(id, kOpenId, sizeof(kOpenId)) == 0) {
    if (LogOn()) REXLOG_INFO("[apt] PC SETTINGS chosen");
    pcs::RequestMenu();
    ctx.r3.u64 = 0;
    return;
  }
  if (std::strncmp(id, kTextId, sizeof(kTextId)) == 0) {
    static const uint32_t text = [] {
      const char* s = "PC SETTINGS";
      uint32_t a = GuestAlloc(64);
      uint8_t* membase = REX_KERNEL_MEMORY()->virtual_membase();
      for (uint32_t i = 0; a && s[i]; ++i) rex::memory::store_and_swap<uint16_t>(rex::memory::GuestPtr(membase, a + 2 * i), uint16_t(s[i]));
      return a;
    }();
    if (text) {
      ctx.r3.u64 = text;
      return;
    }
  }
  __imp__sub_822BDF30(ctx, base);
}
