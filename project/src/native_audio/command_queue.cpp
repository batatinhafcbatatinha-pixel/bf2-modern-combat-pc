// The game's audio command stream: crash fix and monitoring.
//
// The game thread queues audio commands (parameter changes, voice starts, ... records of 8-16
// bytes) into a stream owned by the audio system object, and the game's own audio thread
// (sub_820F3828) runs them in sub_820E3160 once per mixed buffer. Writers append with no bounds
// check: stream base at +36, capacity at +40 (128 KB), write offset at +44, high-water mark at +48.
// On the console the mixer thread keeps up, so a flush sees a few KB at most. Here, when the
// recompiled mixer falls behind (heavy combat), the stream grows past its 128 KB and overwrites the
// heap behind it - the voice table among others - and the main thread later crashes writing
// through a pointer taken from that garbage (guest 0x00000000 / 0x820E3480).
//
// The fix gives the stream room for far longer stalls (no command is dropped and nothing about the
// mix changes); the flush hook reports how close the stream gets to its capacity.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/bf2mc_init.h"

#include "../platform/native_byteorder.h"

namespace {

constexpr uint32_t kStreamGrowth = 16;          // 128 KB -> 2 MB
constexpr uint32_t kMaxStreamBytes = 4u << 20;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return native_host::ByteSwap32(v);
}

std::atomic<uint32_t> g_capacity{0};

}  // namespace

// sub_820E29C0(allocator, stream_capacity): constructs the audio system object; the stream is
// allocated from the game allocator with the requested capacity.
REX_HOOK_RAW(sub_820E29C0) {
  const uint64_t allocator = ctx.r3.u64, capacity = ctx.r4.u64;
  const uint32_t larger = std::min<uint32_t>(uint32_t(capacity) * kStreamGrowth, kMaxStreamBytes);
  ctx.r4.u64 = larger;
  __imp__sub_820E29C0(ctx, base);
  if (ctx.r3.u32) {
    g_capacity = larger;
    REXLOG_INFO("bf2audio: command stream {} KB (game asked {} KB)", larger >> 10, uint32_t(capacity) >> 10);
    return;
  }
  // The game allocator could not provide it: construct exactly as the game asked.
  ctx.r3.u64 = allocator;
  ctx.r4.u64 = capacity;
  __imp__sub_820E29C0(ctx, base);
  g_capacity = uint32_t(capacity);
  REXLOG_WARN("bf2audio: larger command stream unavailable, keeping {} KB", uint32_t(capacity) >> 10);
}

// sub_820E3160(system): runs and clears the queued commands (the audio thread, once per buffer).
REX_HOOK_RAW(sub_820E3160) {
  using Clock = std::chrono::steady_clock;
  static Clock::time_point last_flush = Clock::now(), last_report = Clock::now();
  static uint32_t worst_bytes = 0;
  static double worst_gap_ms = 0.0;
  const uint32_t pending = Load32(base, ctx.r3.u32 + 44);
  const auto now = Clock::now();
  worst_gap_ms = std::max(worst_gap_ms, std::chrono::duration<double, std::milli>(now - last_flush).count());
  worst_bytes = std::max(worst_bytes, pending);
  last_flush = now;
  const uint32_t capacity = g_capacity.load();
  // Past the game's own 128 KB this would have been the crash.
  if (pending > (128u << 10)) {
    static std::atomic<int> warned{0};
    if (warned.fetch_add(1) < 20)
      REXLOG_WARN("bf2audio: command stream reached {} KB of {} KB (audio thread fell behind)", pending >> 10,
                  capacity >> 10);
  }
  if (now - last_report >= std::chrono::seconds(30)) {
    REXLOG_INFO("bf2audio: last 30 s: command stream peak {} KB, longest gap between flushes {:.1f} ms",
                worst_bytes >> 10, worst_gap_ms);
    worst_bytes = 0;
    worst_gap_ms = 0.0;
    last_report = now;
  }
  __imp__sub_820E3160(ctx, base);
}
