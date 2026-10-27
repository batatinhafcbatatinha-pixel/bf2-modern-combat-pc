#include "../platform/native_byteorder.h"
// bf2mc native GPU - replacements for the GPU-facing functions of the game's static Direct3D
// library (addresses and semantics: analysis/d3d-map.md, analysis/native-gpu.md).
//
// No GPU emulator is loaded (ReXApp runs in detached mode, gpu_plugin is cleared in bf2mc_app.h):
// - The D3D device runs in its built-in "GPU-less" mode (device byte 0x28C1 bit 2, the state the
//   library itself enters after a GPU hang): kickoffs are skipped and fences are written by the CPU
//   when inserted. Command buffers are still built by the original code but never executed.
// - Draw / clear / resolve / swap call the original function (CPU-side bookkeeping: shader
//   binding patches the vertex shader microcode, dirty masks, resource fences) and then the host
//   Direct3D 11 renderer, which works from the D3D device's register shadow.
// - The display controller (vblank interrupt, a few status registers the library reads) is
//   provided here; GPU callbacks the game inserts into the command stream are delivered right away
//   on the interrupt thread, since host rendering is synchronous.

#include "native_gpu.h"
#include "../pc_settings.h"

#include "generated/default/bf2mc_init.h"

#include <rex/chrono/clock.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/thread.h>

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <deque>
#include <mutex>

namespace {

constexpr uint32_t kInterruptHandler = 0x82482E80;  // D3D's VdSetGraphicsInterruptCallback target

std::atomic<uint32_t> g_device{0};
std::atomic<bool> g_vblank_running{false};
std::mutex g_callback_mutex;
struct GpuCallback {
  uint32_t function;
  uint32_t data;
};
std::deque<GpuCallback> g_callbacks;
bf2n::UpDraw g_up;
uint8_t* g_membase = nullptr;
void* g_hwnd = nullptr;
bool g_renderer_ready = false;

uint8_t* Mem() {
  if (!g_membase) g_membase = rex::system::kernel_state()->memory()->virtual_membase();
  return g_membase;
}
uint32_t Load32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Mem() + a + (a >= 0xE0000000u ? 0x1000u : 0u), 4);
  return native_host::ByteSwap32(v);
}
void Store32(uint32_t a, uint32_t v) {
  v = native_host::ByteSwap32(v);
  std::memcpy(Mem() + a + (a >= 0xE0000000u ? 0x1000u : 0u), &v, 4);
}

void EnsureRenderer() {
  if (!g_renderer_ready && g_hwnd) g_renderer_ready = bf2n::RendererInit(g_hwnd);
}

// --- Display controller registers (0x7FC80000) -------------------------------------------------
// The library reads only a handful of display/status registers; nothing here executes commands.

uint32_t MmioRead(void*, void*, uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;
  switch (r) {
    case 0x0F00: return 0x08100748;  // RB_EDRAM_TIMING
    case 0x0F01: return 0x0000200E;  // RB_BC_CONTROL
    case 0x194C: {                   // D1MODE_V_COUNTER: same clock as display interrupts
      auto now = std::chrono::steady_clock::now().time_since_epoch();
      uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
      const uint64_t period_us = uint64_t(1e6 / pcs::GuestRefreshHz());
      return uint32_t((us % period_us) * 750 / period_us);
    }
    case 0x1951: return 1;                    // interrupt status: vblank
    case 0x1961: return (1280u << 16) | 720u;  // D1MODE_VIEWPORT_SIZE
    default: {
      static std::atomic<int> logged{0};
      if (logged.fetch_add(1) < 32) REXLOG_INFO("bf2native: display reg read {:04X}", r);
      return 0;
    }
  }
}

void MmioWrite(void*, void*, uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;
  static std::atomic<int> logged{0};
  if (r != 0x1844 && r != 0x01C5 && logged.fetch_add(1) < 32) {
    REXLOG_INFO("bf2native: display reg write {:04X} = {:08X}", r, value);
  }
}

// --- Interrupt thread: 60 Hz vblank + immediate delivery of GPU callbacks ----------------------

void StartInterruptThread() {
  if (g_vblank_running.exchange(true)) return;
  auto* ks = rex::system::kernel_state();
  auto* thread = new rex::system::XHostThread(ks, 128 * 1024, 0, []() -> int {
    auto* self = rex::system::XThread::GetCurrentThread();
    self->SetActiveCpu(2);
    auto* dispatcher = rex::system::kernel_state()->function_dispatcher();
    auto next = std::chrono::steady_clock::now();
    while (g_vblank_running.load()) {
      // The rate can change while playing (settings menu): read it every tick.
      const auto period = std::chrono::microseconds(int64_t(1e6 / pcs::GuestRefreshHz()));
      uint32_t device = g_device.load();
      // GPU callbacks (InsertCallback): the hardware writes {function, data} into the callback
      // block and raises a CP interrupt (source 1) once it reaches them.
      for (;;) {
        GpuCallback cb;
        {
          std::lock_guard<std::mutex> lock(g_callback_mutex);
          if (g_callbacks.empty()) break;
          cb = g_callbacks.front();
          g_callbacks.pop_front();
        }
        uint32_t block = Load32(device + bf2n::dev::kGpuCallbackBlock);
        if (block) {
          Store32(block + 16, cb.function);
          Store32(block + 20, cb.data);
          uint64_t args[] = {1, device};
          dispatcher->ExecuteInterrupt(self->thread_state(), kInterruptHandler, args, 2);
        }
      }
      auto now = std::chrono::steady_clock::now();
      if (now >= next) {
        next += period;
        if (now - next > period * 4) next = now + period;
        uint64_t args[] = {0, device};  // source 0: vblank
        dispatcher->ExecuteInterrupt(self->thread_state(), kInterruptHandler, args, 2);
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  });
  thread->set_name("bf2 VBlank");
  thread->Create();
  REXLOG_INFO("bf2native: interrupt thread started (device {:08X})", g_device.load());
}

}  // namespace

namespace bf2n {

void NativeGpuInstallMmio() {
  auto* memory = rex::system::kernel_state()->memory();
  memory->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, nullptr, MmioRead,
                                MmioWrite);
  REXLOG_INFO("bf2native: display registers mapped, no GPU plugin");
}

void NativeGpuSetWindow(void* hwnd) { g_hwnd = hwnd; }

}  // namespace bf2n

// --- Device creation ---------------------------------------------------------------------------

// sub_82486178(device, params): device initialization (EDRAM, ring buffer, engines, state).
REX_HOOK_RAW(sub_82486178) {
  uint32_t device = ctx.r3.u32;
  g_device = device;
  uint8_t* flags = Mem() + device + bf2n::dev::kFlags10433;
  *flags |= 0x04;  // GPU-less mode: CPU-written fences, no kickoff
  __imp__sub_82486178(ctx, base);
  *flags |= 0x04;
  REXLOG_INFO("bf2native: D3D device {:08X} initialized (GPU-less command buffers)", device);
  EnsureRenderer();
}

// sub_82485BE8(device): VdInitializeEngines + VdSetGraphicsInterruptCallback(0x82482E80, device).
REX_HOOK_RAW(sub_82485BE8) {
  g_device = ctx.r3.u32;
  __imp__sub_82485BE8(ctx, base);
  ctx.r3.u64 = 1;
  StartInterruptThread();
}

// sub_824841C8(device, type, function, data): D3D InsertCallback.
REX_HOOK_RAW(sub_824841C8) {
  GpuCallback cb{ctx.r5.u32, ctx.r6.u32};
  __imp__sub_824841C8(ctx, base);
  if (cb.function) {
    std::lock_guard<std::mutex> lock(g_callback_mutex);
    g_callbacks.push_back(cb);
  }
}

// --- Shaders -----------------------------------------------------------------------------------

// sub_8248AB00(device, patch, shader, ..., declaration [r7]): loads the vertex shader through the
// command buffer (IM_LOAD_IMMEDIATE) and patches that copy (see ImmediateVertexShader); the copy
// ends at the command buffer pointer (device+0) on return.
REX_HOOK_RAW(sub_8248AB00) {
  uint32_t device = ctx.r3.u32, shader = ctx.r5.u32, declaration = ctx.r7.u32;
  uint32_t before = Load32(device);
  __imp__sub_8248AB00(ctx, base);
  uint32_t end = Load32(device);
  static const bool disabled = std::getenv("BF2_NO_IMMEDIATE_VS") != nullptr;  // diagnostics
  if (disabled || end == before || !shader || !declaration) return;
  uint32_t size = Load32(shader + 0x258);
  uint32_t start = end - size + 4;
  if (!size || size > 0x10000 || (size & 3)) return;
  auto& imm = bf2n::LoadedImmediateVertexShader();
  imm.device = device;
  imm.shader = shader;
  imm.declaration = declaration;
  imm.declaration_id = Load32(declaration + 32);
  const uint8_t* src = Mem() + start + (start >= 0xE0000000u ? 0x1000u : 0u);
  imm.code.assign(src, src + size);
  ++imm.generation;
  static std::atomic<int> logs{0};
  if (logs.fetch_add(1) < 8)
    REXLOG_INFO("bf2native: vertex shader {:08X} loaded through the command buffer ({} bytes at {:08X})", shader,
                size, start);
}

// --- Draws -------------------------------------------------------------------------------------

// sub_8247CBE0(device, prim, base_vertex, start_index, index_count): DrawIndexedVertices.
REX_HOOK_RAW(sub_8247CBE0) {
  uint32_t device = ctx.r3.u32, prim = ctx.r4.u32, start = ctx.r6.u32, count = ctx.r7.u32;
  int32_t base_vertex = ctx.r5.s32;
  __imp__sub_8247CBE0(ctx, base);
  EnsureRenderer();
  if (g_renderer_ready) bf2n::RendererDraw(device, prim, base_vertex, start, count, true);
}

// sub_8247C9A0(device, prim, start_vertex, vertex_count): DrawVertices.
REX_HOOK_RAW(sub_8247C9A0) {
  uint32_t device = ctx.r3.u32, prim = ctx.r4.u32, start = ctx.r5.u32, count = ctx.r6.u32;
  __imp__sub_8247C9A0(ctx, base);
  EnsureRenderer();
  if (g_renderer_ready) bf2n::RendererDraw(device, prim, int32_t(start), 0, count, false);
}

// sub_8247C288(device, prim, vertex_count, stride) -> vertex pointer: BeginVertices.
REX_HOOK_RAW(sub_8247C288) {
  bf2n::UpDraw up;
  up.prim = ctx.r4.u32;
  up.vertex_count = ctx.r5.u32;
  up.stride = ctx.r6.u32;
  __imp__sub_8247C288(ctx, base);
  up.vertices = ctx.r3.u32;
  up.pending = up.vertices != 0;
  g_up = up;
}

// sub_8247C5B0(device, prim, base_vertex, vertex_count, index_count, index_format, stride,
//              out_indices, [stack] out_vertices): BeginIndexedVertices.
REX_HOOK_RAW(sub_8247C5B0) {
  bf2n::UpDraw up;
  up.indexed = true;
  up.prim = ctx.r4.u32;
  up.base_vertex = ctx.r5.s32;
  up.vertex_count = ctx.r6.u32;
  up.index_count = ctx.r7.u32;
  up.index32 = ctx.r8.u32 == 1;
  up.stride = ctx.r9.u32;
  uint32_t out_indices = ctx.r10.u32;
  uint32_t out_vertices = Load32(ctx.r1.u32 + 0x54);
  __imp__sub_8247C5B0(ctx, base);
  if (ctx.r3.u32 == 0 && out_indices && out_vertices) {
    up.indices = Load32(out_indices);
    up.vertices = Load32(out_vertices);
    up.pending = up.indices && up.vertices;
  }
  g_up = up;
}

static void FlushUp(uint32_t device) {
  if (!g_up.pending) return;
  g_up.pending = false;
  EnsureRenderer();
  if (g_renderer_ready) bf2n::RendererDrawUp(device, g_up);
}

// sub_8247C508(device): EndVertices / EndIndexedVertices.
REX_HOOK_RAW(sub_8247C508) {
  uint32_t device = ctx.r3.u32;
  __imp__sub_8247C508(ctx, base);
  FlushUp(device);
}

// sub_8247C528(device, prim, count, data, stride): DrawVerticesUP.
REX_HOOK_RAW(sub_8247C528) {
  uint32_t device = ctx.r3.u32;
  __imp__sub_8247C528(ctx, base);
  FlushUp(device);
}

// sub_8247C8C0: DrawIndexedVerticesUP.
REX_HOOK_RAW(sub_8247C8C0) {
  uint32_t device = ctx.r3.u32;
  __imp__sub_8247C8C0(ctx, base);
  FlushUp(device);
}

// --- Clear / Resolve / Swap --------------------------------------------------------------------

// sub_82474310(device, rect_count, rects, flags, color, f1 = z, r9 = stencil): Clear.
REX_HOOK_RAW(sub_82474310) {
  uint32_t device = ctx.r3.u32, count = ctx.r4.u32, rects = ctx.r5.u32, flags = ctx.r6.u32;
  uint32_t color = ctx.r7.u32, stencil = ctx.r9.u32;
  float z = float(ctx.f1.f64);
  EnsureRenderer();
  if (g_renderer_ready) bf2n::RendererClear(device, count, rects, flags, color, z, stencil);
  __imp__sub_82474310(ctx, base);
}

// sub_82484E48(device, flags, src_rect, dest_texture, dest_point, level, slice, clear_color,
//              f1 = clear_z, [stack] clear_stencil, params): Resolve.
REX_HOOK_RAW(sub_82484E48) {
  uint32_t device = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, dest = ctx.r6.u32;
  uint32_t point = ctx.r7.u32, level = ctx.r8.u32, slice = ctx.r9.u32;
  uint32_t clear_color = ctx.r10.u32;
  float z = float(ctx.f1.f64);
  // Stack parameter slots are 8 bytes from r1+0x50; clear_z takes the 9th slot (float args also
  // consume a GPR slot), clear_stencil is the 10th (low word at +0x5C, see the caller 8234AE40).
  uint32_t stencil = Load32(ctx.r1.u32 + 0x5C);
  EnsureRenderer();
  if (g_renderer_ready) {
    bf2n::RendererResolve(device, flags, rect, dest, point, level, slice, clear_color, z, stencil);
  }
  __imp__sub_82484E48(ctx, base);
}

// sub_82472D10(device, front_buffer_texture, params): Swap.
REX_HOOK_RAW(sub_82472D10) {
  uint32_t device = ctx.r3.u32, front = ctx.r4.u32;
  EnsureRenderer();
  if (g_renderer_ready) bf2n::RendererSwap(device, front);
  __imp__sub_82472D10(ctx, base);
}
