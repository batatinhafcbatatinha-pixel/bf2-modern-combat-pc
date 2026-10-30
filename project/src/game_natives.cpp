// Native replacements for hot translated game functions, with the translated code's results.
//
// sub_82349150 writes particle vertices (~14% of the render thread in a mission): batches of at
// most 4000 particles between the vertex writer's begin (sub_8233F140) and end (sub_8233F778);
// per particle a 24-byte record {x, y, z, u, v, alpha} becomes position, UV and a packed colour
// (alpha truncated to an integer, low byte on top of a fixed RGB). The scaffolding (singletons,
// stack descriptor, begin/end calls) is reproduced with the same guest stack frame; the
// per-particle loop is native.
// sub_824AD0A8 normalises a vec3 (dst r3, src r4; zero below an epsilon), ~14% of a worker thread.
// Vector ops use the translated code's operations, order and flush mode.
//
// BF2_NO_NATIVE_PARTICLES=1 / BF2_NO_NATIVE_NORMALIZE=1   translated code only
// BF2_NATIVE_CHECK=1   particles: the translated function runs and every batch it wrote is
//                      compared with the native computation; normalize: both run on the same
//                      input and the outputs are compared ("[native-check] ... MISMATCH")
// BF2_NATIVE_AB=1      alternate native/translated every 600 presented frames and log
//                      "[native-ab]" with frame time and the calling thread's CPU time

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>

REX_EXTERN(__imp__sub_82349150);
REX_EXTERN(__imp__sub_824AD0A8);
REX_EXTERN(__imp__sub_8233F140);
REX_EXTERN(__imp__sub_8233F778);
REX_EXTERN(sub_8233F0D8);  // vertex writer singleton
REX_EXTERN(sub_823486E0);  // particle list singleton
REX_EXTERN(sub_8233D9F0);  // batch descriptor constructor
REX_EXTERN(sub_8233DA50);  // batch descriptor destructor
REX_EXTERN(sub_8233F140);  // begin(writer, 2, descriptor, count, 9, 0, matrix, arg)
REX_EXTERN(sub_8233F778);  // end(writer, descriptor)

namespace bf2_natives {
namespace {
bool EnvSet(const char* name) {
  const char* v = std::getenv(name);
  return v && *v && *v != '0';
}
const bool g_particles_enabled = !EnvSet("BF2_NO_NATIVE_PARTICLES");
const bool g_normalize_enabled = !EnvSet("BF2_NO_NATIVE_NORMALIZE");
const bool g_check = EnvSet("BF2_NATIVE_CHECK");
const bool g_ab = EnvSet("BF2_NATIVE_AB");
std::atomic<bool> g_ab_on{true};
std::atomic<uint64_t> g_particle_batches{0}, g_particle_mismatches{0};
std::atomic<uint64_t> g_normalize_calls{0}, g_normalize_mismatches{0};

double ThreadCpuMs() {
#ifdef _WIN32
  FILETIME c, e, k, u;
  if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
  auto ms = [](FILETIME f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e4; };
  return ms(k) + ms(u);
#else
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
#endif
}
}  // namespace

bool NativeOn() { return !g_ab || g_ab_on.load(std::memory_order_relaxed); }

// Called once per presented frame (gpu_hooks.cpp, Swap).
void FrameTick() {
  if (!g_ab) return;
  static uint64_t frames = 0;
  static auto start = std::chrono::steady_clock::now();
  static double cpu_start = ThreadCpuMs();
  if (++frames % 600 != 0) return;
  auto now = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(now - start).count();
  double cpu = ThreadCpuMs();
  start = now;
  bool was = g_ab_on.load(std::memory_order_relaxed);
  REXLOG_INFO("[native-ab] native={} frames=600 ms={:.1f} fps={:.2f} thread_cpu_ms/frame={:.3f}", was ? 1 : 0, ms,
              600000.0 / ms, (cpu - cpu_start) / 600.0);
  cpu_start = cpu;
  g_ab_on.store(!was, std::memory_order_relaxed);
}
}  // namespace bf2_natives

namespace {
using namespace bf2_natives;
using V = simde__m128i;
using F = simde__m128;

uint8_t* Host(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
uint32_t Load32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Host(base, a), 4);
  return __builtin_bswap32(v);
}
void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Host(base, a), &v, 4);
}
V Rev(V v) { return simde_mm_shuffle_epi8(v, simde_mm_loadu_si128(reinterpret_cast<const V*>(rex::ppc::VectorMaskL))); }
V LoadA(uint8_t* base, uint32_t a) { return Rev(simde_mm_load_si128(reinterpret_cast<const V*>(Host(base, a & ~0xFu)))); }
void StoreA(uint8_t* base, uint32_t a, V v) { simde_mm_store_si128(reinterpret_cast<V*>(Host(base, a & ~0xFu)), Rev(v)); }
F AsF(V v) { return simde_mm_castsi128_ps(v); }
V AsV(F f) { return simde_mm_castps_si128(f); }

// ---------------------------------------------------------------------------------- particles
constexpr uint32_t kParticleRgb = 0x828164C5;   // 3 bytes: R, G, B
constexpr uint32_t kZero = 0x8209890C;          // float written into the descriptor (+0xA0..+0xA8)

// fctidz of a float alpha, low 32 bits (stfiwx).
uint32_t AlphaWord(uint32_t bits) {
  float f;
  std::memcpy(&f, &bits, 4);
  double d = double(f);
  int64_t i = std::isnan(d) ? int64_t(0x8000000000000000ULL)
              : (d > double(LLONG_MAX)) ? LLONG_MAX
                                         : simde_mm_cvttsd_si64(simde_mm_load_sd(&d));
  return uint32_t(i);
}

// The per-particle loop of 0x823492B0: `count` records from `src`, vertex writer `writer`.
// `stack_word` receives the last alpha word (the original keeps it at r1+80).
void WriteParticles(uint8_t* base, uint32_t writer, uint32_t src, int32_t count, uint32_t rgb,
                    uint32_t stack_word) {
  uint32_t alpha = 0;
  for (int32_t i = 0; i < count; ++i, src += 24) {
    alpha = AlphaWord(Load32(base, src + 12));
    uint32_t v = Load32(base, writer + 60) + Load32(base, writer + 8);
    uint32_t z = Load32(base, src + 0), y = Load32(base, src - 4), x = Load32(base, src - 8);
    Store32(base, v, x);
    Store32(base, v + 4, y);
    Store32(base, v + 8, z);
    v = Load32(base, writer + 60) + Load32(base, writer + 28);
    uint32_t tv = Load32(base, src + 8), tu = Load32(base, src + 4);
    Store32(base, v, tu);
    Store32(base, v + 4, tv);
    Store32(base, Load32(base, writer + 60) + Load32(base, writer + 48), ((alpha & 0xFF) << 24) | rgb);
    Store32(base, writer + 60, Load32(base, writer + 60) + Load32(base, writer + 4));
  }
  if (count > 0) Store32(base, stack_word, alpha);
}

uint32_t ParticleRgb(uint8_t* base) {
  const uint8_t* c = Host(base, kParticleRgb);
  return (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | c[2];
}

void ParticleVertices(PPCContext& ctx, uint8_t* base) {
  const uint32_t obj = ctx.r3.u32, matrix_src = ctx.r4.u32;
  const uint64_t arg = ctx.r5.u64;
  const uint32_t old_sp = ctx.r1.u32;
  const uint64_t old_lr = ctx.lr;
  const uint32_t sp = old_sp - 336;
  Store32(base, sp, old_sp);  // stwu r1,-336(r1)
  ctx.r1.u32 = sp;
  ctx.lr = 0x8234916C;
  sub_8233F0D8(ctx, base);
  const uint32_t writer = ctx.r3.u32;
  if (*Host(base, obj) != 0 && *Host(base, obj + 1) == 0) {
    ctx.r3.u64 = sp + 96;
    ctx.lr = 0x82349190;
    sub_8233D9F0(ctx, base);
    Store32(base, sp + 108, 1);
    Store32(base, sp + 112, 0);
    Store32(base, sp + 128, 0);
    Store32(base, sp + 116, 6);
    Store32(base, sp + 120, 7);
    Store32(base, sp + 96, 42);
    for (uint32_t i = 0; i < 16; ++i) Store32(base, sp + 208 + 4 * i, Load32(base, matrix_src + 4 * i));
    const int32_t frame = int32_t(Load32(base, obj + 68) + 1);
    ctx.fpscr.disableFlushMode();
    const uint32_t zero = Load32(base, kZero);
    Store32(base, sp + 256, zero);
    Store32(base, sp + 260, zero);
    Store32(base, sp + 264, zero);
    const int32_t buffer = frame % 2;
    ctx.lr = 0x82349210;
    sub_823486E0(ctx, base);
    const uint32_t list = Load32(base, ctx.r3.u32);
    Store32(base, sp + 144, list);
    if (list) {
      const int32_t total = int32_t(Load32(base, obj + uint32_t((buffer + 19) << 2)));
      const uint32_t records = Load32(base, obj + uint32_t((buffer + 21) << 2));
      int32_t done = 0;
      while (total > 0) {
        int32_t n = total - done;
        if (n >= 4000) n = 4000;
        ctx.r3.u64 = writer;
        ctx.r4.u64 = 2;
        ctx.r5.u64 = sp + 96;
        ctx.r6.s64 = n;
        ctx.r7.u64 = 9;
        ctx.r8.u64 = 0;
        ctx.r9.u64 = sp + 208;
        ctx.r10.u64 = arg;
        ctx.lr = 0x82349288;
        sub_8233F140(ctx, base);
        if (n > 0) {
          const uint32_t src = uint32_t(done) * 24 + records + 8;
          const uint32_t rgb = ParticleRgb(base);
          done += n;
          ctx.fpscr.disableFlushMode();
          WriteParticles(base, writer, src, n, rgb, sp + 80);
        }
        ctx.r3.u64 = writer;
        ctx.r4.u64 = sp + 96;
        ctx.lr = 0x82349350;
        sub_8233F778(ctx, base);
        if (!(done < total)) break;
      }
    }
    ctx.r3.u64 = sp + 96;
    ctx.lr = 0x82349360;
    sub_8233DA50(ctx, base);
  }
  ctx.r1.u32 = old_sp;
  ctx.lr = old_lr;  // restored by the epilogue
}

// Check mode for the particles: the translated function runs; begin/end of each of its batches
// are watched and the batch's vertices are compared with WriteParticles on the same records.
struct ParticleCheck {
  bool active = false;
  uint32_t writer = 0, records = 0, done = 0, vertex = 0;
  int32_t count = 0;
};
thread_local ParticleCheck t_check;

void CheckBatch(uint8_t* base) {
  ParticleCheck& c = t_check;
  if (c.count <= 0) return;
  const uint32_t stride = Load32(base, c.writer + 4), o_pos = Load32(base, c.writer + 8),
                 o_uv = Load32(base, c.writer + 28), o_col = Load32(base, c.writer + 48);
  const uint32_t rgb = ParticleRgb(base);
  bool same = Load32(base, c.writer + 60) == c.vertex + uint32_t(c.count) * stride;
  uint32_t src = c.done * 24 + c.records + 8, v = c.vertex;
  for (int32_t i = 0; same && i < c.count; ++i, src += 24, v += stride) {
    same = Load32(base, v + o_pos) == Load32(base, src - 8) && Load32(base, v + o_pos + 4) == Load32(base, src - 4) &&
           Load32(base, v + o_pos + 8) == Load32(base, src) && Load32(base, v + o_uv) == Load32(base, src + 4) &&
           Load32(base, v + o_uv + 4) == Load32(base, src + 8) &&
           Load32(base, v + o_col) == (((AlphaWord(Load32(base, src + 12)) & 0xFF) << 24) | rgb);
  }
  uint64_t batches = g_particle_batches.fetch_add(1) + 1;
  if (!same) {
    uint64_t m = g_particle_mismatches.fetch_add(1) + 1;
    if (m <= 20) REXLOG_WARN("[native-check] particles MISMATCH writer={:08X} count={}", c.writer, c.count);
  }
  if ((batches & 0x3FF) == 0)
    REXLOG_INFO("[native-check] particles batches={} mismatches={}", batches, g_particle_mismatches.load());
  c.done += uint32_t(c.count);
  c.count = 0;
}

// ---------------------------------------------------------------------------------- normalize
constexpr uint32_t kEpsilon = 0x82038920;

void Normalize(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, sp = ctx.r1.u32;
  // lvx/lvx/lvsl/vperm: 16 bytes at src from the blocks holding src and src+11.
  const V lo = LoadA(base, src);
  const V shift = simde_mm_load_si128(reinterpret_cast<const V*>(&rex::ppc::VectorShiftTableL[(src & 0xF) * 16]));
  Store32(base, sp + 20, dst);
  ctx.fpscr.disableFlushMode();
  const uint32_t eps = Load32(base, kEpsilon);
  for (uint32_t i = 0; i < 4; ++i) Store32(base, sp - 16 + 4 * i, eps);
  const V hi = LoadA(base, src + 11);
  const F v = AsF(rex::ppc::simde_mm_perm_epi8_(lo, hi, shift));
  ctx.fpscr.enableFlushModeUnconditional();
  const F len2 = simde_mm_dp_ps(v, v, 0xEF);
  const F e = AsF(LoadA(base, sp - 16));
  F r;
  if (simde_mm_movemask_ps(simde_mm_cmpge_ps(e, len2)) == 0xF) {
    r = AsF(simde_mm_set1_epi32(0));
  } else {
    ctx.fpscr.enableFlushMode();
    F rs = simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(len2));
    const F half = simde_mm_mul_ps(simde_mm_cvtepi32_ps(simde_mm_set1_epi32(1)), simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x3F000000))));
    const F rs2 = simde_mm_mul_ps(rs, rs);
    const F h = simde_mm_mul_ps(len2, half);
    const F corr = simde_mm_xor_ps(simde_mm_sub_ps(simde_mm_mul_ps(h, rs2), half), simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000))));
    rs = simde_mm_add_ps(simde_mm_mul_ps(rs, corr), rs);
    StoreA(base, sp - 16, AsV(rs));
    r = simde_mm_mul_ps(v, rs);
  }
  alignas(16) uint32_t lanes[4];
  simde_mm_store_si128(reinterpret_cast<V*>(lanes), AsV(r));
  Store32(base, dst & ~3u, lanes[3]);
  Store32(base, (Load32(base, sp + 20) + 4) & ~3u, lanes[2]);
  Store32(base, (Load32(base, sp + 20) + 8) & ~3u, lanes[1]);
  ctx.r3.u64 = Load32(base, sp + 20);
}
}  // namespace

REX_HOOK_RAW(sub_82349150) {
  if (g_particles_enabled && NativeOn()) {
    if (!g_check) {
      ParticleVertices(ctx, base);
      return;
    }
    // Check: remember where this call's records are, then let the translated code run.
    const uint32_t obj = ctx.r3.u32;
    const int32_t buffer = int32_t(Load32(base, obj + 68) + 1) % 2;
    t_check = ParticleCheck{true, 0, Load32(base, obj + uint32_t((buffer + 21) << 2)), 0, 0, 0};
    __imp__sub_82349150(ctx, base);
    t_check.active = false;
    return;
  }
  __imp__sub_82349150(ctx, base);
}

REX_HOOK_RAW(sub_8233F140) {
  const uint32_t writer = ctx.r3.u32;
  const int32_t count = ctx.r6.s32;
  __imp__sub_8233F140(ctx, base);
  if (t_check.active) {
    t_check.writer = writer;
    t_check.count = count;
    t_check.vertex = Load32(base, writer + 60);
  }
}

REX_HOOK_RAW(sub_8233F778) {
  if (t_check.active) CheckBatch(base);
  __imp__sub_8233F778(ctx, base);
}

REX_HOOK_RAW(sub_824AD0A8) {
  if (!g_normalize_enabled || !NativeOn()) {
    __imp__sub_824AD0A8(ctx, base);
    return;
  }
  if (!g_check) {
    Normalize(ctx, base);
    return;
  }
  const uint32_t dst = ctx.r3.u32, sp = ctx.r1.u32;
  uint8_t before_dst[12], before_lo[16], before_hi[4];
  std::memcpy(before_dst, Host(base, dst & ~3u), 12);
  std::memcpy(before_lo, Host(base, sp - 16), 16);
  std::memcpy(before_hi, Host(base, sp + 20), 4);
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64;
  Normalize(ctx, base);
  uint8_t native_dst[12], native_lo[16];
  std::memcpy(native_dst, Host(base, dst & ~3u), 12);
  std::memcpy(native_lo, Host(base, sp - 16), 16);
  const uint64_t native_r3 = ctx.r3.u64;
  std::memcpy(Host(base, dst & ~3u), before_dst, 12);
  std::memcpy(Host(base, sp - 16), before_lo, 16);
  std::memcpy(Host(base, sp + 20), before_hi, 4);
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  __imp__sub_824AD0A8(ctx, base);
  const bool same = std::memcmp(Host(base, dst & ~3u), native_dst, 12) == 0 &&
                    std::memcmp(Host(base, sp - 16), native_lo, 16) == 0 && ctx.r3.u64 == native_r3;
  const uint64_t calls = g_normalize_calls.fetch_add(1) + 1;
  if (!same) {
    uint64_t m = g_normalize_mismatches.fetch_add(1) + 1;
    if (m <= 20) REXLOG_WARN("[native-check] normalize MISMATCH dst={:08X} src={:08X}", dst, uint32_t(r4));
  }
  if ((calls & 0xFFFFF) == 0)
    REXLOG_INFO("[native-check] normalize calls={} mismatches={}", calls, g_normalize_mismatches.load());
}
