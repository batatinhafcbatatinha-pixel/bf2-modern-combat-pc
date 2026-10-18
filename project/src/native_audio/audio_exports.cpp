// Kernel audio exports implemented natively (see xma_engine.h and audio_system.h). These
// definitions take the place of the SDK's xboxkrnl audio exports, which drive its APU model; the
// game's import thunks call straight into them. Battlefield 2 uses all 19 XMA* imports and the
// XAudio render driver; it has no statically linked XMAPlayback library.

#include <atomic>
#include <cstring>
#include <thread>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xtypes.h>

#include "generated/default/bf2mc_init.h"
#include "audio_system.h"
#include "xma_engine.h"
#include "xma_fields.h"

using namespace rex;
using bf2audio::Ctx;
using bf2audio::XmaEngine;
namespace xf = bf2audio;

namespace {

inline uint32_t Load32(uint8_t* base, uint32_t a) { return rex::memory::load_and_swap<uint32_t>(rex::memory::GuestPtr(base, a)); }
inline void Store32(uint8_t* base, uint32_t a, uint32_t v) { rex::memory::store_and_swap<uint32_t>(rex::memory::GuestPtr(base, a), v); }
inline uint16_t Load16(uint8_t* base, uint32_t a) { return rex::memory::load_and_swap<uint16_t>(rex::memory::GuestPtr(base, a)); }
inline void Store16(uint8_t* base, uint32_t a, uint16_t v) { rex::memory::store_and_swap<uint16_t>(rex::memory::GuestPtr(base, a), v); }

uint32_t Physical(uint32_t virtual_address) {
  return virtual_address ? REX_KERNEL_MEMORY()->GetPhysicalAddress(virtual_address) : 0;
}

int ContextIndex(uint32_t context) {
  int index = XmaEngine::Get().IndexOf(context);
  if (index < 0) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) REXLOG_WARN("[native-xma] call with a foreign context {:08X}", context);
  }
  return index;
}

// Field read/modify on a context while no decode pass runs.
template <typename Fn>
void EditContext(uint32_t context, Fn&& fn) {
  int index = ContextIndex(context);
  if (index < 0) return;
  XmaEngine::Get().Edit(index, [&](uint8_t* p) {
    Ctx c;
    c.Load(p);
    fn(c);
    c.Store(p);
  });
}

uint32_t ReadField(uint8_t* base, uint32_t context, xf::Field f) {
  Ctx c;
  c.Load(rex::memory::GuestPtr(base, context));
  return c[f];
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// XMA*
// ---------------------------------------------------------------------------------------------

// XMACreateContext(u32* out_context)
REX_HOOK_RAW(__imp__XMACreateContext) {
  uint32_t context = XmaEngine::Get().Allocate();
  Store32(base, ctx.r3.u32, context);
  ctx.r3.u64 = context ? X_STATUS_SUCCESS : X_STATUS_NO_MEMORY;
}

REX_HOOK_RAW(__imp__XMAReleaseContext) {
  XmaEngine::Get().Release(ctx.r3.u32);
  ctx.r3.u64 = 0;
}

// XMAInitializeContext(context, XMA_CONTEXT_INIT*)
REX_HOOK_RAW(__imp__XMAInitializeContext) {
  uint32_t context = ctx.r3.u32, init = ctx.r4.u32;
  int index = ContextIndex(context);
  if (index < 0) {
    ctx.r3.u64 = X_E_FALSE;
    return;
  }
  // XMA_CONTEXT_INIT: in0, in0 packets, in1, in1 packets, read offset, out, out blocks, work buffer,
  // subframe decode count, channel count - 1, sample rate id, loop start/end/count/subframe end/skip.
  uint32_t f[11];
  for (int i = 0; i < 11; i++) f[i] = Load32(base, init + i * 4);
  uint32_t loop_start = Load32(base, init + 44), loop_end = Load32(base, init + 48);
  const uint8_t* loop = rex::memory::GuestPtr(base, init + 52);
  uint8_t loop_count = loop[0], loop_subframe_end = loop[1], loop_subframe_skip = loop[2];
  XmaEngine::Get().Edit(index, [&](uint8_t* p) {
    Ctx c{};
    c.d[xf::kIn0Ptr] = Physical(f[0]);
    c.set(xf::kIn0Packets, f[1]);
    c.d[xf::kIn1Ptr] = Physical(f[2]);
    c.set(xf::kIn1Packets, f[3]);
    c.set(xf::kInReadOffset, f[4]);
    c.d[xf::kOutPtr] = Physical(f[5]);
    c.set(xf::kOutBlocks, f[6]);
    c.set(xf::kSubframeDecodeCount, f[8]);
    c.set(xf::kStereo, f[9] >= 1);
    c.set(xf::kSampleRate, f[10]);
    c.set(xf::kLoopStart, loop_start);
    c.set(xf::kLoopEnd, loop_end);
    c.set(xf::kLoopCount, loop_count);
    c.set(xf::kLoopSubframeEnd, loop_subframe_end);
    c.set(xf::kLoopSubframeSkip, loop_subframe_skip);
    c.Store(p);
  });
  // Initialization also resets the decoder's offsets and validity, as the context clear does.
  XmaEngine::Get().Clear(index);
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XMASetInputBuffer0) {
  uint32_t buffer = Physical(ctx.r4.u32), packets = ctx.r5.u32;
  EditContext(ctx.r3.u32, [&](Ctx& c) {
    c.d[xf::kIn0Ptr] = buffer;
    c.set(xf::kIn0Packets, packets);
  });
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XMASetInputBuffer1) {
  uint32_t buffer = Physical(ctx.r4.u32), packets = ctx.r5.u32;
  EditContext(ctx.r3.u32, [&](Ctx& c) {
    c.d[xf::kIn1Ptr] = buffer;
    c.set(xf::kIn1Packets, packets);
  });
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XMAIsInputBuffer0Valid) { ctx.r3.u64 = ReadField(base, ctx.r3.u32, xf::kIn0Valid); }
REX_HOOK_RAW(__imp__XMAIsInputBuffer1Valid) { ctx.r3.u64 = ReadField(base, ctx.r3.u32, xf::kIn1Valid); }
REX_HOOK_RAW(__imp__XMAIsOutputBufferValid) { ctx.r3.u64 = ReadField(base, ctx.r3.u32, xf::kOutValid); }
REX_HOOK_RAW(__imp__XMAGetOutputBufferReadOffset) { ctx.r3.u64 = ReadField(base, ctx.r3.u32, xf::kOutRead); }
REX_HOOK_RAW(__imp__XMAGetOutputBufferWriteOffset) { ctx.r3.u64 = ReadField(base, ctx.r3.u32, xf::kOutWrite); }

REX_HOOK_RAW(__imp__XMASetInputBuffer0Valid) {
  EditContext(ctx.r3.u32, [](Ctx& c) { c.set(xf::kIn0Valid, 1); });
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XMASetInputBuffer1Valid) {
  EditContext(ctx.r3.u32, [](Ctx& c) { c.set(xf::kIn1Valid, 1); });
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XMASetOutputBufferValid) {
  EditContext(ctx.r3.u32, [](Ctx& c) { c.set(xf::kOutValid, 1); });
  ctx.r3.u64 = 0;
}

// XMASetInputBufferReadOffset(context, bit offset)
REX_HOOK_RAW(__imp__XMASetInputBufferReadOffset) {
  uint32_t offset = ctx.r4.u32;
  EditContext(ctx.r3.u32, [&](Ctx& c) { c.set(xf::kInReadOffset, offset); });
  ctx.r3.u64 = 0;
}

// XMASetLoopData(context, XMA_LOOP_DATA*): loop start, loop end (bits), count, subframe end, skip.
REX_HOOK_RAW(__imp__XMASetLoopData) {
  uint32_t data = ctx.r4.u32;
  uint32_t loop_start = Load32(base, data), loop_end = Load32(base, data + 4);
  const uint8_t* loop = rex::memory::GuestPtr(base, data + 8);
  uint8_t loop_count = loop[0], loop_subframe_end = loop[1], loop_subframe_skip = loop[2];
  EditContext(ctx.r3.u32, [&](Ctx& c) {
    c.set(xf::kLoopStart, loop_start);
    c.set(xf::kLoopEnd, loop_end);
    c.set(xf::kLoopCount, loop_count);
    c.set(xf::kLoopSubframeEnd, loop_subframe_end);
    c.set(xf::kLoopSubframeSkip, loop_subframe_skip);
  });
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XMASetOutputBufferReadOffset) {
  uint32_t offset = ctx.r4.u32;
  EditContext(ctx.r3.u32, [&](Ctx& c) { c.set(xf::kOutRead, offset); });
  ctx.r3.u64 = 0;
}

// Enabling a context starts decoding it (a pass now, then the engine's background sweep).
REX_HOOK_RAW(__imp__XMAEnableContext) {
  XmaEngine::Get().Kick(ContextIndex(ctx.r3.u32));
  ctx.r3.u64 = 0;
}

// XMADisableContext(context, wait): no pass may run on the context once this returns successfully;
// without `wait` it fails while a pass is running.
REX_HOOK_RAW(__imp__XMADisableContext) {
  bool done = XmaEngine::Get().Disable(ContextIndex(ctx.r3.u32), ctx.r4.u32 != 0);
  ctx.r3.u64 = done ? X_E_SUCCESS : X_E_FALSE;
}

// XMABlockWhileInUse(context): returns once no decode pass is using the context.
REX_HOOK_RAW(__imp__XMABlockWhileInUse) {
  XmaEngine::Get().Lock(ContextIndex(ctx.r3.u32), true);
  ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------------------------------
// XAudio render driver
// ---------------------------------------------------------------------------------------------

// Clients whose voice category volumes were reported (see XAudioGetVoiceCategoryVolumeChangeMask).
static std::atomic<uint32_t> g_volume_reported{0};

REX_HOOK_RAW(__imp__XAudioGetSpeakerConfig) {
  Store32(base, ctx.r3.u32, 0x00010001);
  ctx.r3.u64 = 0;
}

// XAudioRegisterRenderDriverClient(u32 callback_and_arg[2], u32* out_driver)
REX_HOOK_RAW(__imp__XAudioRegisterRenderDriverClient) {
  uint32_t callback_ptr = ctx.r3.u32, driver_ptr = ctx.r4.u32;
  auto* audio = bf2audio::AudioSystem();
  uint32_t callback = callback_ptr ? Load32(base, callback_ptr) : 0;
  if (!audio || !callback) {
    ctx.r3.u64 = X_E_INVALIDARG;
    return;
  }
  uint32_t index = 0;
  X_STATUS status = audio->RegisterClient(callback, Load32(base, callback_ptr + 4), &index);
  if (status != X_STATUS_SUCCESS) {
    ctx.r3.u64 = status;
    return;
  }
  Store32(base, driver_ptr, 0x41550000 | index);
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XAudioUnregisterRenderDriverClient) {
  g_volume_reported &= ~(1u << (ctx.r3.u32 & 31));
  if (auto* audio = bf2audio::AudioSystem()) audio->UnregisterClient(ctx.r3.u32 & 0xFFFF);
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XAudioSubmitRenderDriverFrame) {
  if (auto* audio = bf2audio::AudioSystem()) audio->SubmitFrame(ctx.r3.u32 & 0xFFFF, ctx.r4.u32);
  ctx.r3.u64 = 0;
}

// No system-wide voice category volumes exist on PC: full volume, never changed afterwards. The
// game's XAudio (sub_8248E7F8) only reads a category volume (2 categories) when its bit is set in
// the change mask, so the first query of each client reports both as changed to load the 1.0.
REX_HOOK_RAW(__imp__XAudioGetVoiceCategoryVolumeChangeMask) {
  std::this_thread::yield();
  uint32_t client = 1u << (ctx.r3.u32 & 31);
  bool first = !(g_volume_reported.fetch_or(client) & client);
  Store32(base, ctx.r4.u32, first ? 0x3 : 0);
  ctx.r3.u64 = 0;
}

REX_HOOK_RAW(__imp__XAudioGetVoiceCategoryVolume) {
  float one = 1.0f;
  uint32_t bits;
  std::memcpy(&bits, &one, 4);
  Store32(base, ctx.r4.u32, bits);
  ctx.r3.u64 = 0;
}
