// The game's own menus on PC: the main menu's "XBOX LIVE GAME" line (the online multiplayer) reads
// "ONLINE GAME". The game's texts are UTF-16 tables (Localisation\Ingame.loc, packed in
// Data1.viv), read with NtReadFile; the line is renamed in the buffer the game reads into.
// bf2mc-pc.ini [opcoes] menu_no_jogo=0 keeps the original.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef _WIN32
#include "platform/native_win32_posix.h"
#endif

#include <algorithm>
#include <atomic>
#include <functional>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include "pc_settings.h"

namespace {

using Thunk = void (*)(PPCContext&, uint8_t*);

// The SDK's own export thunk (rexruntime.dll), called around our additions.
Thunk SdkExport(const char* name) {
  HMODULE runtime = GetModuleHandleW(L"rexruntime.dll");
  if (!runtime) runtime = GetModuleHandleW(L"rexruntimerd.dll");
  return runtime ? reinterpret_cast<Thunk>(GetProcAddress(runtime, name)) : nullptr;
}

uint32_t Load32(uint8_t* base, uint32_t a) { return rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, a)); }
uint16_t Load16(uint8_t* base, uint32_t a) { return rex::memory::load_and_swap<uint16_t>(rex::memory::GuestPtr(base, a)); }

std::mutex g_mu;
std::set<uint32_t> g_loc_handles;  // open handles of the files holding the texts

// Replaces the UTF-16BE text `from` (whole entry) with `to`, NUL-padded, in buf[0..len).
int RenameUtf16(uint8_t* buf, uint32_t len, const char* from, const char* to) {
  std::vector<uint8_t> f, t;  // UTF-16BE of ASCII text
  for (const char* c = from; *c; ++c) f.insert(f.end(), {0, uint8_t(*c)});
  for (const char* c = to; *c; ++c) t.insert(t.end(), {0, uint8_t(*c)});
  int n = 0;
  const std::boyer_moore_horspool_searcher searcher(f.begin(), f.end());
  uint8_t* end = buf + len;
  for (uint8_t* p = buf; (p = std::search(p, end, searcher)) != end; p += f.size()) {
    size_t i = size_t(p - buf);
    if (i + f.size() + 2 > len || buf[i + f.size()] || buf[i + f.size() + 1]) continue;
    if (i >= 2 && (buf[i - 2] || buf[i - 1])) continue;  // whole entries only
    std::memcpy(p, t.data(), t.size());
    std::memset(p + t.size(), 0, f.size() - t.size());
    ++n;
  }
  return n;
}

}  // namespace

// NtCreateFile(handle_out, access, OBJECT_ATTRIBUTES*, io_status, allocation, attributes, share,
// disposition, options). OBJECT_ATTRIBUTES +4 -> ANSI_STRING {u16 length, u16 max, u32 buffer}.
REX_HOOK_RAW(__imp__NtCreateFile) {
  static const Thunk sdk = SdkExport("__imp__NtCreateFile");
  const uint32_t handle_out = ctx.r3.u32, attributes = ctx.r5.u32;
  std::string path;
  if (attributes) {
    uint32_t name = Load32(base, attributes + 4);
    if (name) {
      uint16_t length = Load16(base, name);
      uint32_t text = Load32(base, name + 4);
      if (text) path.assign(reinterpret_cast<const char*>(rex::memory::GuestPtr(base, text)), length);
    }
  }
  sdk(ctx, base);
  auto ends = [&](const char* tail) {
    size_t n = std::strlen(tail);
    return path.size() >= n && _stricmp(path.c_str() + path.size() - n, tail) == 0;
  };
  if (ctx.r3.u32 == 0 && handle_out && pcs::GameMenuEntry() && (ends("ingame.loc") || ends("data1.viv"))) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_loc_handles.insert(Load32(base, handle_out));
  }
}

// NtReadFile(handle, event, apc, apc_context, io_status, buffer, length, offset)
REX_HOOK_RAW(__imp__NtReadFile) {
  static const Thunk sdk = SdkExport("__imp__NtReadFile");
  const uint32_t handle = ctx.r3.u32, buffer = ctx.r8.u32, length = ctx.r9.u32;
  sdk(ctx, base);
  bool loc;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    loc = g_loc_handles.count(handle) != 0;
  }
  if (loc && buffer && (ctx.r3.u32 == 0 || ctx.r3.u32 == 0x103)) {  // success or pending (done at once)
    int n = RenameUtf16(rex::memory::GuestPtr(base, buffer), length, "XBOX LIVE GAME", "ONLINE GAME");
    if (n) REXLOG_INFO("[menu] XBOX LIVE GAME reads ONLINE GAME ({} copies)", n);
  }
}

REX_HOOK_RAW(__imp__NtClose) {
  static const Thunk sdk = SdkExport("__imp__NtClose");
  {
    std::lock_guard<std::mutex> lock(g_mu);
    g_loc_handles.erase(ctx.r3.u32);
  }
  sdk(ctx, base);
}
