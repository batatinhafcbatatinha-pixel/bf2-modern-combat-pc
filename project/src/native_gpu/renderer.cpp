// bf2mc native GPU - host Direct3D 11 renderer (interface: native_gpu.h).
//
// Called from the guest D3D hooks (gpu_hooks.cpp) after the original function has done its CPU-side
// bookkeeping. Every draw reads the D3D device's register shadow (render state, constants, fetch
// constants, shaders) and is issued directly through Direct3D 11:
// - Shaders: Xenos microcode -> HLSL (xenos_hlsl.cpp) -> D3DCompile, cached in memory and on disk.
// - Vertex data: a 512 MB raw buffer mirrors guest physical memory; the ranges a draw's fetch
//   constants reference are hashed in 16 KB chunks and re-uploaded when the CPU changed them.
// - Indices: native strips keep their restart markers; fans, quads and line loops are converted
//   to lists. Rectangle lists and points expand in a GS.
// - EDRAM: each (EDRAM base, format, size) render target is a host texture. Resolve copies into a
//   host texture keyed by the destination's physical address, which then takes the place of the
//   guest texture when sampled (the guest memory is never written: there is no GPU).
// - Swap presents the resolved front buffer, letterboxed, to a flip-model swap chain on the window.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include "native_gpu.h"
#include "pc_game_info.h"
#include "textures.h"
#include "cached_state.h"
#include "parallel_jobs.h"
#include "shader_compile.h"
#include "xenos_hlsl.h"

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <xxhash.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bf2n {
namespace {

using Microsoft::WRL::ComPtr;

// Optional host CPU phase timings; nested phases are reported separately.
struct PerfScope {
  static bool Enabled() { static bool on = std::getenv("BF2_PERF") != nullptr; return on; }
  static inline double ms[13]{};
  static inline uint64_t calls[13]{};
  int slot; bool active; std::chrono::steady_clock::time_point start;
  explicit PerfScope(int i) : slot(i), active(Enabled()) { if (active) start = std::chrono::steady_clock::now(); }
  void Stop() { if (active) { ms[slot] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count(); ++calls[slot]; active = false; } }
  ~PerfScope() { Stop(); }
  static void Report(uint32_t frame) {
    if (!Enabled() || frame % 120) return;
    REXLOG_INFO("bf2perf: frame {} draw {:.3f} mirror {:.3f} texture {:.3f} late {:.3f} constants {:.3f} shader {:.3f} resolve {:.3f} ms/frame; constant-update calls {}", frame, ms[0]/120, ms[1]/120, ms[2]/120, ms[3]/120, ms[4]/120, ms[5]/120, ms[6]/120, calls[4]);
    REXLOG_INFO("bf2detail: frame {} texture_hash {:.3f} decode {:.3f} upload {:.3f} prepare {:.3f} bindings {:.3f} pipeline {:.3f} ms/frame; decoded {} textures/120frames", frame, ms[7]/120, ms[8]/120, ms[9]/120, ms[10]/120, ms[11]/120, ms[12]/120, calls[8]);
    for (int i=0;i<13;++i) { ms[i]=0; calls[i]=0; }
  }
};

constexpr uint32_t kPhysicalSize = 0x20000000u;
constexpr uint32_t kChunkShift = 14;  // 16 KB mirror chunks
constexpr uint32_t kChunkCount = kPhysicalSize >> kChunkShift;
// Staging pool for the mirror uploads of one frame (see SyncMirror / LateSyncMirror).
constexpr uint32_t kPoolSize = 48u << 20;
// Chunks that changed this recently are re-checked when the frame executes.
constexpr uint32_t kLateSyncFrames = 120;
constexpr uint32_t kIndexRingCount = 4u << 20;  // indices

// --- Guest memory ----------------------------------------------------------------------------

auto* Mem() { return rex::system::kernel_state()->memory(); }
inline uint32_t Be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return _byteswap_ulong(v);
}
inline uint16_t Be16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return _byteswap_ushort(v);
}
inline uint8_t* Virt(uint32_t a) { return Mem()->TranslateVirtual(a); }
inline uint32_t LoadV(uint32_t a) { return Be32(Virt(a)); }
inline float AsFloat(uint32_t v) {
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline uint32_t AsUint(float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  return v;
}
uint32_t PhysOf(uint32_t virt) {
  if (virt < kPhysicalSize) return virt;
  uint32_t p = Mem()->GetPhysicalAddress(virt);
  return p == UINT32_MAX ? (virt & 0x1FFFFFFFu) : p;
}

// Register shadow inside the D3D device (analysis/d3d-map.md).
uint32_t Reg(uint32_t device, uint32_t r) {
  static const struct {
    uint32_t first, count, offset;
  } kBlocks[] = {{0x2000, 0x10, 0x2CC0}, {0x2100, 0x15, 0x2D0C}, {0x2180, 0x05, 0x2D60},
                 {0x2200, 0x0C, 0x2D74}, {0x2280, 0x15, 0x2DA4}, {0x2300, 0x26, 0x2DF8},
                 {0x2380, 0x08, 0x2E90}};
  for (const auto& b : kBlocks) {
    if (r >= b.first && r < b.first + b.count) return LoadV(device + b.offset + (r - b.first) * 4);
  }
  return 0;
}
float RegF(uint32_t device, uint32_t r) { return AsFloat(Reg(device, r)); }

// Texture headers (and some fetch constants) hold CPU virtual addresses of physical memory;
// everything host-side is keyed by guest physical address.
void NormalizeFetch(TexFetch& f) {
  if (uint32_t base = f.d[1] & 0xFFFFF000u; base >= kPhysicalSize) {
    f.d[1] = (f.d[1] & 0xFFFu) | (PhysOf(base) & 0x1FFFF000u);
  }
  if (uint32_t mip = f.d[5] & 0xFFFFF000u; mip >= kPhysicalSize) {
    f.d[5] = (f.d[5] & 0xFFFu) | (PhysOf(mip) & 0x1FFFF000u);
  }
}

TexFetch ReadFetch(uint32_t addr) {
  TexFetch f;
  for (int i = 0; i < 6; ++i) f.d[i] = LoadV(addr + i * 4);
  NormalizeFetch(f);
  return f;
}

// --- Utility shaders ---------------------------------------------------------------------------

const char kUtilHlsl[] = R"(
cbuffer Util : register(b0) { float4 u0; float4 u1; float4 u2; float4 u3; };
struct FsOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
FsOut vs_fs(uint id : SV_VertexID) {
  FsOut o;
  float2 t = float2((id << 1) & 2, id & 2);
  o.uv = t;
  o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), u0.x, 1.0);
  return o;
}
struct ClearOut {
  float4 c0 : SV_Target0; float4 c1 : SV_Target1; float4 c2 : SV_Target2; float4 c3 : SV_Target3;
  float d : SV_Depth;
};
ClearOut ps_clear(FsOut i) {
  ClearOut o;
  o.c0 = u1; o.c1 = u1; o.c2 = u1; o.c3 = u1;
  o.d = u0.x;
  return o;
}
Texture2D<float4> src : register(t0);
SamplerState lin : register(s0);
float4 ps_copy(FsOut i) : SV_Target {
  int2 p = int2(i.pos.xy) + int2(u2.xy);
  float4 v = src.Load(int3(p, 0));
  return u2.z != 0.0 ? v.bgra : v;
}
float4 ps_present(FsOut i) : SV_Target {
  float4 v = src.SampleLevel(lin, i.uv * u3.xy, 0.0);
  return float4(u3.z != 0.0 ? v.bgr : v.rgb, 1.0);
}
)";

const char kGsHlsl[] = R"(
cbuffer XState : register(b1) {
  uint4 xb[2];
  uint4 xl[8];
  uint4 xf[48];
  float4 xsys[8];
};
struct XVSOut {
  float4 pos : SV_Position;
  float4 o[16] : TEXCOORD0;
  float4 pts : TEXCOORD16;
};
[maxvertexcount(4)]
void gs_point(point XVSOut v[1], inout TriangleStream<XVSOut> s) {
  float2 size = v[0].pts.x > 0.0 ? v[0].pts.xx : xsys[5].zw;
  float2 h = size * 0.5 * xsys[1].zw * v[0].pos.w;
  const float2 c[4] = {float2(-1.0, 1.0), float2(1.0, 1.0), float2(-1.0, -1.0), float2(1.0, -1.0)};
  [unroll] for (int i = 0; i < 4; ++i) {
    XVSOut o = v[0];
    o.pos.xy += c[i] * h;
    o.pts.zw = c[i] * float2(0.5, -0.5) + 0.5;
    s.Append(o);
  }
}
[maxvertexcount(4)]
void gs_rect(triangle XVSOut v[3], inout TriangleStream<XVSOut> s) {
  float2 p0 = v[0].pos.xy / v[0].pos.w, p1 = v[1].pos.xy / v[1].pos.w, p2 = v[2].pos.xy / v[2].pos.w;
  float d01 = dot(p1 - p0, p1 - p0), d12 = dot(p2 - p1, p2 - p1), d20 = dot(p0 - p2, p0 - p2);
  int c = 2, a = 0, b = 1;
  if (d12 >= d01 && d12 >= d20) { c = 0; a = 1; b = 2; }
  else if (d20 >= d01) { c = 1; a = 2; b = 0; }
  s.Append(v[c]);
  s.Append(v[a]);
  s.Append(v[b]);
  XVSOut d;
  d.pos = v[a].pos + v[b].pos - v[c].pos;
  [unroll] for (int i = 0; i < 16; ++i) d.o[i] = v[a].o[i] + v[b].o[i] - v[c].o[i];
  d.pts = v[a].pts + v[b].pts - v[c].pts;
  s.Append(d);
}
)";

// --- Renderer state ----------------------------------------------------------------------------

struct XState {
  uint32_t xb[8];
  uint32_t xl[32];
  uint32_t xf[192];
  uint32_t xsys[kSysFloat4Count * 4];
};
static_assert(sizeof(XState) % 16 == 0);

struct UtilConsts {
  float u0[4], u1[4], u2[4], u3[4];
};

struct HostRT {
  ComPtr<ID3D11Texture2D> tex;
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11RenderTargetView> rtv_gamma;  // 8_8_8_8_GAMMA view of an 8_8_8_8 target
  ComPtr<ID3D11DepthStencilView> dsv;
  ComPtr<ID3D11ShaderResourceView> srv;
  uint32_t width = 0, height = 0;
  uint32_t guest_format = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  bool depth = false;
  float scale = 1.0f;  // host texels per guest texel (internal resolution)
};

struct Resolved {
  ComPtr<ID3D11Texture2D> tex;
  ComPtr<ID3D11ShaderResourceView> srv;
  ComPtr<ID3D11ShaderResourceView> srv_slice0;  // Texture2D view of slice 0 (present, dumps)
  std::vector<ComPtr<ID3D11RenderTargetView>> rtvs;  // per array slice
  uint32_t width = 0, height = 0, slices = 1;
  uint32_t guest_format = 0;
  bool cube = false;
  float scale = 1.0f;  // that of the targets resolved into it (copies are texel for texel)
  uint32_t used_width = 0, used_height = 0;  // extent of the last copy into it
  bool bgra = false;  // holds an 8_8_8_8 resolve, stored as BGRA (see Resolve)
  uint64_t signature = 0;  // guest memory signature at resolve time
  uint32_t checked_frame = 0;
};

struct HostTex {
  ComPtr<ID3D11Resource> res;
  ComPtr<ID3D11ShaderResourceView> srv;
  uint64_t mem_hash = 0;
  uint32_t checked_frame = 0;
  TexDim dim = TexDim::k2D;
  bool failed = false;
  uint32_t levels = 1;
  TexFetch fetch;
};

struct VsEntry {
  ComPtr<ID3D11VertexShader> vs;
  ShaderInfo info;
  uint64_t key = 0;
};
struct PsEntry {
  ComPtr<ID3D11PixelShader> ps;
  ShaderInfo info;
  uint64_t key = 0;
};

struct ChunkState {
  uint64_t hash = 0;
  uint32_t checked_frame = 0;
  uint32_t checked_draw = 0;
  uint32_t changed_frame = 0;
  uint32_t used_frame = UINT32_MAX;  // last frame a draw referenced the chunk
  uint32_t captures = 0;             // uploads recorded in used_frame
  uint32_t pool_offset = UINT32_MAX; // pool copy of the single capture, if any
  bool valid = false;
};

struct Targets {
  HostRT* color[4] = {};
  bool gamma[4] = {};
  HostRT* depth = nullptr;
  uint32_t width = 0, height = 0;
  float scale = 1.0f;  // internal resolution of these targets
};

enum class Kind { kPoints, kLines, kTris, kRects };

// Trace lines (BF2_TRACE_FRAME / BF2_DEBUG_AT / F12 capture) go to the log and, during an F12
// capture, to the capture's trace.txt.
#define BF2_TRACE(...) TraceLine(fmt::format(__VA_ARGS__))

class Renderer {
 public:
  bool Init(HWND hwnd);
  void Draw(uint32_t device, uint32_t prim, std::vector<uint32_t>& vids, const UpDraw* up);
  void Clear(uint32_t device, uint32_t count, uint32_t rects, uint32_t flags, uint32_t color,
             float z, uint32_t stencil);
  void Resolve(uint32_t device, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
               uint32_t dest_point, uint32_t level, uint32_t slice, uint32_t clear_color,
               float clear_z, uint32_t clear_stencil);
  void Swap(uint32_t device, uint32_t front_texture);

  std::mutex mutex;

 private:
  bool CreateUtility();
  ComPtr<ID3DBlob> Compile(const std::string& source, const char* entry, const char* target);
  VsEntry* GetVertexShader(uint32_t object);
  PsEntry* GetPixelShader(uint32_t object, uint32_t param_gen_reg);
  bool ReadUcode(uint32_t object, bool pixel, std::vector<uint32_t>& out);

  HostRT* GetRT(uint32_t base, uint32_t format, uint32_t w, uint32_t h, bool depth);
  bool CurrentTargets(uint32_t device, Targets& t, bool want_color = true);
  void BindTargets(const Targets& t, uint32_t color_mask);

  void SyncMirror(uint32_t address, uint32_t size, bool force);
  void LateSyncMirror();
  ID3D11ShaderResourceView* GetTexture(const TexFetch& f, TexDim want);
  ID3D11ShaderResourceView* GetResolvedSrv(const TexFetch& f, TexDim want);
  ID3D11SamplerState* GetSampler(const TexFetch& f, const TextureBinding& b);
  Resolved* GetResolved(const TexFetch& f, float scale);
  uint64_t GuestSignature(const TexFetch& f);

  ID3D11BlendState* GetBlend(const D3D11_BLEND_DESC& d);
  ID3D11DepthStencilState* GetDepthStencil(const D3D11_DEPTH_STENCIL_DESC& d);
  ID3D11RasterizerState* GetRasterizer(const D3D11_RASTERIZER_DESC& d);
  ID3D11SamplerState* GetSamplerState(const D3D11_SAMPLER_DESC& d);

  void UpdateBuffer(ID3D11Buffer* buffer, const void* data, size_t size) {
    UpdateBuffer(ctx_.Get(), buffer, data, size);
  }
  void UpdateBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* buffer, const void* data, size_t size);
  void RefreshTextures();
  void SetUtil(const UtilConsts& u);
  void ClearTargetsWithDraw(const Targets& t, const D3D11_RECT& rect, uint32_t flags,
                            const float color[4], float z, uint32_t stencil);

  HWND hwnd_ = nullptr;
  ComPtr<ID3D11Device> dev_;
  ComPtr<ID3D11DeviceContext> imm_;  // immediate: command list execution + present
  ComPtr<ID3D11DeviceContext> ctx_;  // deferred: records the guest frame
  bool buggy_deferred_update_ = false;  // runtime-emulated command lists (UpdateSubresource box bug)
  std::vector<uint64_t> frame_textures_;  // texture cache keys sampled this frame
  ParallelJobs texture_hash_jobs_;
  std::vector<HostTex*> texture_hash_inputs_;
  std::vector<uint64_t> texture_hash_results_;
  ComPtr<IDXGISwapChain1> swap_;
  ComPtr<ID3D11RenderTargetView> back_rtv_;
  uint32_t back_w_ = 0, back_h_ = 0;

  ComPtr<ID3D11Buffer> mirror_;
  ComPtr<ID3D11ShaderResourceView> mirror_srv_;
  std::vector<ChunkState> chunks_;
  // Mirror uploads are recorded as copies from pool_, whose contents (pool_cpu_) are uploaded
  // when the frame executes; frame_chunks_ lists the chunks referenced by this frame's draws.
  ComPtr<ID3D11Buffer> pool_;
  std::vector<uint8_t> pool_cpu_;
  uint32_t pool_used_ = 0;
  std::vector<uint32_t> frame_chunks_;
  ComPtr<ID3D11Buffer> vs_consts_, ps_consts_, state_cb_, util_cb_;
  // Only these three buffers are exclusively written on the deferred context.
  // Byte comparisons avoid hash collisions and preserve all shader constants exactly.
  struct ConstantSnapshot {
    std::vector<uint8_t> bytes;
    bool valid = false;
  };
  ConstantSnapshot constant_snapshots_[3];
  struct PipelineCache {
    CachedState<ID3D11BlendState*> blend;
    CachedState<std::array<float,4>> blend_factor;
    CachedState<ID3D11DepthStencilState*> depth;
    CachedState<uint32_t> stencil;
    CachedState<ID3D11RasterizerState*> raster;
    CachedState<D3D11_VIEWPORT> viewport;
    CachedState<D3D11_RECT> scissor;
    CachedState<D3D11_PRIMITIVE_TOPOLOGY> topology;
    CachedState<ID3D11VertexShader*> vs;
    CachedState<ID3D11GeometryShader*> gs;
    CachedState<ID3D11PixelShader*> ps;
    bool fixed_bindings = false;
  } pipeline_cache_;
  ComPtr<ID3D11Buffer> index_ring_;
  uint32_t index_pos_ = kIndexRingCount;  // first deferred map must discard

  ComPtr<ID3D11VertexShader> vs_fs_;
  ComPtr<ID3D11PixelShader> ps_clear_, ps_copy_, ps_present_;
  ComPtr<ID3D11GeometryShader> gs_point_, gs_rect_;
  ComPtr<ID3D11SamplerState> linear_clamp_;
  ComPtr<ID3D11ShaderResourceView> black_2d_, black_3d_, black_cube_;

  std::unordered_map<uint64_t, std::unique_ptr<VsEntry>> vs_cache_;
  std::unordered_map<uint64_t, std::unique_ptr<PsEntry>> ps_cache_;
  std::unordered_map<uint64_t, HostRT> rts_;
  std::unordered_map<uint32_t, Resolved> resolved_;
  std::unordered_map<uint64_t, HostTex> textures_;
  std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> blend_cache_;
  std::unordered_map<uint64_t, ComPtr<ID3D11DepthStencilState>> ds_cache_;
  std::unordered_map<uint64_t, ComPtr<ID3D11RasterizerState>> rs_cache_;
  std::unordered_map<uint64_t, ComPtr<ID3D11SamplerState>> sampler_cache_;

  std::filesystem::path cache_dir_;
  uint32_t frame_ = 1;
  // Internal resolution (PC settings, fixed at boot): render targets at least 640 wide (the scene,
  // half-resolution effects, shadow maps) and what is resolved from them are scale_ times their
  // guest size; smaller ones (reduction chains, probes) keep it. Everything else stays in guest
  // pixels.
  float scale_ = 1.0f;
  float ScaleFor(uint32_t width) const { return width >= 640 ? scale_ : 1.0f; }
  static uint32_t ScaledSize(uint32_t v, float s) {
    return std::max<uint32_t>(1, uint32_t(std::ceil(float(v) * s - 0.001f)));
  }
  static LONG ScaledCoord(LONG v, float s) { return LONG(std::lround(float(v) * s)); }
  static D3D11_RECT ScaledRect(const D3D11_RECT& r, float s) {
    return {ScaledCoord(r.left, s), ScaledCoord(r.top, s), ScaledCoord(r.right, s), ScaledCoord(r.bottom, s)};
  }
  uint32_t draw_serial_ = 1;
  uint32_t frame_draws_ = 0, frame_skipped_ = 0;
  uint32_t last_front_base_ = 0;
  std::chrono::steady_clock::time_point last_present_{};
  uint32_t trace_frame_ = 0;
  // BF2_DEBUG_AT=<seconds>: trace + dump targets of the first frame after that time.
  uint32_t debug_at_sec_ = 0;
  std::chrono::steady_clock::time_point init_time_{};
  std::vector<uint64_t> skip_shaders_;
  // BF2_STEP_REGION=x,y,w,h: in the traced frame, snapshot that region of RT0 after every draw;
  // at Swap the draws that changed it are logged and their snapshots dumped (step_<draw>.bmp).
  D3D11_BOX step_box_{};
  bool step_on_ = false;
  struct StepArray {
    ComPtr<ID3D11Texture2D> tex;
    DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
    uint32_t used = 0;
  };
  struct Step {
    uint32_t draw;
    const HostRT* rt;
    DXGI_FORMAT format;
    uint32_t slice;
    uint64_t array_key;
  };
  std::map<uint64_t, StepArray> step_arrays_;
  std::vector<Step> steps_;
  // Traced frame: pipeline statistics of every draw (pixels shaded per draw), read after it runs.
  std::vector<std::pair<uint32_t, ComPtr<ID3D11Query>>> draw_stats_;
  void SaveSteps();
  bool skip_toggle_ = false;
  bool dump_hlsl_ = false;  // BF2_DUMP_HLSL: write each translated shader next to the cache
  void DumpHlsl(const char* kind, uint32_t object, const std::string& hlsl) {
    if (!dump_hlsl_) return;
    char name[48];
    snprintf(name, sizeof(name), "%s_%08X.hlsl", kind, object);
    std::ofstream(cache_dir_ / name, std::ios::binary) << hlsl;
  }  // BF2_TRACE_FRAME=<n>: log every draw of frame n
  uint32_t dump_every_ = 0;  // BF2_DUMP_EVERY=<frames>: write the presented image to a .bmp
  uint32_t dump_targets_frame_ = 0;  // BF2_DUMP_RESOLVES=<n>: write every target of frame n
  void DumpBackBuffer();
  void LogCenterTexel(ID3D11Texture2D* tex, const char* name);
  void DumpSrv(ID3D11ShaderResourceView* srv, uint32_t w, uint32_t h, const std::string& name);
  void WriteBmp(const std::string& name, const uint8_t* rgba, uint32_t pitch, uint32_t w,
                uint32_t h);

  // F12 capture (RequestCapture): the next frame is traced into captures/<time>/trace.txt with
  // every render target, resolve and sampled texture (each mip level) and the final picture.
 public:
  std::atomic<bool> capture_request{false};

 private:
  std::filesystem::path out_dir_;          // where dumps go: next to the exe, or the capture
  struct UpDump { uint32_t draw, vertices, bytes; };
  std::vector<UpDump> up_dumps_;           // traced frame: UP vertex data saved at draw time
  uint32_t capture_frame_ = UINT32_MAX;
  std::ofstream trace_out_;
  void TraceLine(const std::string& line) {
    REXLOG_INFO("{}", line);
    if (trace_out_.is_open()) trace_out_ << line << '\n';
  }
  void BeginCapture();
  void DumpFrameTextures();
};

Renderer* g_renderer = nullptr;

template <typename T>
uint64_t HashOf(const T& v) {
  return XXH3_64bits(&v, sizeof(T));
}

// --- Initialization ----------------------------------------------------------------------------

bool Renderer::Init(HWND hwnd) {
  hwnd_ = hwnd;
  UINT flags = 0;
  if (GetEnvironmentVariableA("BF2_D3D_DEBUG", nullptr, 0)) flags |= D3D11_CREATE_DEVICE_DEBUG;
  // PC settings: graphics API (Direct3D 11 or Vulkan) and card chosen in the settings menu.
  HRESULT hr = pcs::CreateDevice(flags, &dev_, &imm_);
  scale_ = pcs::ResolutionScale();
  if (scale_ != 1.0f) REXLOG_INFO("bf2native: internal resolution x{:.3f}", scale_);
  if (FAILED(hr)) {
    REXLOG_ERROR("bf2native: D3D11CreateDevice failed {:08X}", uint32_t(hr));
    return false;
  }
  // The guest frame is recorded on a deferred context and executed at Swap, like the console GPU
  // consuming the command buffer after the CPU finished the frame (CPU-written textures such as
  // video frames are complete by then).
  if (FAILED(dev_->CreateDeferredContext(0, &ctx_))) {
    REXLOG_ERROR("bf2native: CreateDeferredContext failed");
    return false;
  }
  D3D11_FEATURE_DATA_THREADING threading{};
  dev_->CheckFeatureSupport(D3D11_FEATURE_THREADING, &threading, sizeof(threading));
  buggy_deferred_update_ = !threading.DriverCommandLists;

  ComPtr<IDXGIDevice> dxgi_dev;
  ComPtr<IDXGIAdapter> adapter;
  ComPtr<IDXGIFactory2> factory;
  dev_.As(&dxgi_dev);
  dxgi_dev->GetAdapter(&adapter);
  adapter->GetParent(IID_PPV_ARGS(&factory));
  DXGI_ADAPTER_DESC ad{};
  adapter->GetDesc(&ad);
  char name[128] = {};
  WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);

  DXGI_SWAP_CHAIN_DESC1 sd{};
  sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sd.Flags = pcs::SwapChainFlags();
  sd.Scaling = DXGI_SCALING_STRETCH;
  hr = factory->CreateSwapChainForHwnd(dev_.Get(), hwnd, &sd, nullptr, nullptr, &swap_);
  if (FAILED(hr)) {
    REXLOG_ERROR("bf2native: CreateSwapChainForHwnd failed {:08X}", uint32_t(hr));
    return false;
  }
  factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

  // Guest physical memory mirror for vertex fetches.
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = kPhysicalSize;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
  hr = dev_->CreateBuffer(&bd, nullptr, &mirror_);
  if (FAILED(hr)) {
    REXLOG_ERROR("bf2native: 512 MB vertex mirror buffer failed {:08X}", uint32_t(hr));
    return false;
  }
  D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
  svd.Format = DXGI_FORMAT_R32_TYPELESS;
  svd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
  svd.BufferEx.NumElements = kPhysicalSize / 4;
  svd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
  dev_->CreateShaderResourceView(mirror_.Get(), &svd, &mirror_srv_);
  chunks_.resize(kChunkCount);
  bd.ByteWidth = kPoolSize;
  if (SUCCEEDED(dev_->CreateBuffer(&bd, nullptr, &pool_))) pool_cpu_.resize(kPoolSize);

  auto make_cb = [&](uint32_t size, ComPtr<ID3D11Buffer>& out) {
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = (size + 15) & ~15u;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(dev_->CreateBuffer(&d, nullptr, &out));
  };
  if (!make_cb(4096, vs_consts_) || !make_cb(4096, ps_consts_) ||
      !make_cb(sizeof(XState), state_cb_) || !make_cb(sizeof(UtilConsts), util_cb_)) {
    return false;
  }
  D3D11_BUFFER_DESC ib{};
  ib.ByteWidth = kIndexRingCount * 4;
  ib.Usage = D3D11_USAGE_DYNAMIC;
  ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
  ib.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(dev_->CreateBuffer(&ib, nullptr, &index_ring_))) return false;

  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  cache_dir_ = std::filesystem::path(exe).parent_path() / "native_shader_cache";
  out_dir_ = std::filesystem::path(exe).parent_path();
  std::error_code ec;
  std::filesystem::create_directories(cache_dir_, ec);

  if (!CreateUtility()) return false;
  char env[256] = {};
  if (GetEnvironmentVariableA("BF2_DUMP_EVERY", env, sizeof(env))) dump_every_ = uint32_t(atoi(env));
  if (GetEnvironmentVariableA("BF2_TRACE_FRAME", env, sizeof(env))) trace_frame_ = uint32_t(atoi(env));
  dump_hlsl_ = GetEnvironmentVariableA("BF2_DUMP_HLSL", env, sizeof(env)) != 0;
  skip_toggle_ = GetEnvironmentVariableA("BF2_SKIP_TOGGLE", env, sizeof(env)) != 0;
  if (GetEnvironmentVariableA("BF2_STEP_REGION", env, sizeof(env))) {
    unsigned x, y, w, h;
    if (sscanf_s(env, "%u,%u,%u,%u", &x, &y, &w, &h) == 4 && w && h) {
      step_box_ = D3D11_BOX{x, y, 0, x + w, y + h, 1};
      step_on_ = true;
    }
  }
  if (GetEnvironmentVariableA("BF2_SKIP_SHADER", env, sizeof(env))) {
    for (char* p = env; *p;) {
      char* end = nullptr;
      uint64_t k = _strtoui64(p, &end, 16);
      if (end == p) break;
      skip_shaders_.push_back(k);
      p = *end ? end + 1 : end;
    }
  }
  if (GetEnvironmentVariableA("BF2_DEBUG_AT", env, sizeof(env))) debug_at_sec_ = uint32_t(atoi(env));
  init_time_ = std::chrono::steady_clock::now();
  if (GetEnvironmentVariableA("BF2_DUMP_RESOLVES", env, sizeof(env))) {
    dump_targets_frame_ = uint32_t(atoi(env));
  }
  REXLOG_INFO("bf2native: {} renderer on {} (feature level {:X})", pcs::Vulkan() ? "Vulkan (DXVK)" : "Direct3D 11", name,
              uint32_t(dev_->GetFeatureLevel()));
  return true;
}

bool Renderer::CreateUtility() {
  std::string util = kUtilHlsl, gs = kGsHlsl;
  auto vs = Compile(util, "vs_fs", "vs_5_0");
  auto pc = Compile(util, "ps_clear", "ps_5_0");
  auto pk = Compile(util, "ps_copy", "ps_5_0");
  auto pp = Compile(util, "ps_present", "ps_5_0");
  auto gp = Compile(gs, "gs_point", "gs_5_0");
  auto gr = Compile(gs, "gs_rect", "gs_5_0");
  if (!vs || !pc || !pk || !pp || !gp || !gr) return false;
  dev_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_fs_);
  dev_->CreatePixelShader(pc->GetBufferPointer(), pc->GetBufferSize(), nullptr, &ps_clear_);
  dev_->CreatePixelShader(pk->GetBufferPointer(), pk->GetBufferSize(), nullptr, &ps_copy_);
  dev_->CreatePixelShader(pp->GetBufferPointer(), pp->GetBufferSize(), nullptr, &ps_present_);
  dev_->CreateGeometryShader(gp->GetBufferPointer(), gp->GetBufferSize(), nullptr, &gs_point_);
  dev_->CreateGeometryShader(gr->GetBufferPointer(), gr->GetBufferSize(), nullptr, &gs_rect_);

  D3D11_SAMPLER_DESC s;
  ZeroMemory(&s, sizeof(s));
  s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  s.MaxLOD = D3D11_FLOAT32_MAX;
  s.ComparisonFunc = D3D11_COMPARISON_NEVER;
  dev_->CreateSamplerState(&s, &linear_clamp_);

  // Fallback textures (opaque black) for unsupported / missing fetch constants.
  const uint32_t black[6] = {0xFF000000u, 0xFF000000u, 0xFF000000u,
                             0xFF000000u, 0xFF000000u, 0xFF000000u};
  D3D11_SUBRESOURCE_DATA init[6];
  for (auto& i : init) i = {black, 4, 4};
  {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = d.Height = 1;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> t;
    dev_->CreateTexture2D(&d, init, &t);
    D3D11_SHADER_RESOURCE_VIEW_DESC v{};
    v.Format = d.Format;
    v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    v.Texture2DArray.MipLevels = 1;
    v.Texture2DArray.ArraySize = 1;
    dev_->CreateShaderResourceView(t.Get(), &v, &black_2d_);
    d.ArraySize = 6;
    d.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    ComPtr<ID3D11Texture2D> c;
    dev_->CreateTexture2D(&d, init, &c);
    v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    v.TextureCube.MipLevels = 1;
    v.TextureCube.MostDetailedMip = 0;
    dev_->CreateShaderResourceView(c.Get(), &v, &black_cube_);
  }
  {
    D3D11_TEXTURE3D_DESC d{};
    d.Width = d.Height = d.Depth = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture3D> t;
    dev_->CreateTexture3D(&d, init, &t);
    dev_->CreateShaderResourceView(t.Get(), nullptr, &black_3d_);
  }
  return true;
}

// D3DCompile with access violations inside the compiler turned into E_UNEXPECTED.
HRESULT GuardedCompile(const std::string& source, const char* entry, const char* target,
                       ID3DBlob** blob, ID3DBlob** errors) {
  __try {
    return D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, entry, target,
                      kShaderCompileFlags, 0, blob, errors);
  } __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER
                                                                : EXCEPTION_CONTINUE_SEARCH) {
    return E_UNEXPECTED;
  }
}

ComPtr<ID3DBlob> Renderer::Compile(const std::string& source, const char* entry,
                                   const char* target) {
  uint64_t key = XXH3_64bits(source.data(), source.size()) ^
                 (XXH3_64bits(entry, std::strlen(entry)) * 31) ^
                 (XXH3_64bits(target, std::strlen(target)) * 131) ^
                 (uint64_t(kShaderCompileFlags) * 0x9E3779B185EBCA87ull);
  char name[32];
  snprintf(name, sizeof(name), "%016llx.dxbc", static_cast<unsigned long long>(key));
  std::filesystem::path path = cache_dir_ / name;
  ComPtr<ID3DBlob> blob;
  {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (in) {
      auto size = size_t(in.tellg());
      if (size && SUCCEEDED(D3DCreateBlob(size, &blob))) {
        in.seekg(0);
        in.read(static_cast<char*>(blob->GetBufferPointer()), std::streamsize(size));
        if (in) return blob;
      }
      blob.Reset();
    }
  }
  ComPtr<ID3DBlob> errors;
  // FXC itself can crash on some inputs: keep the source being compiled on disk (removed on
  // return) so a crash leaves it behind, and contain access violations raised inside it.
  std::filesystem::path pending = cache_dir_ / (std::string(name) + ".compiling.hlsl");
  {
    std::ofstream dump(pending, std::ios::binary);
    dump << source;
  }
  HRESULT hr = GuardedCompile(source, entry, target, blob.ReleaseAndGetAddressOf(),
                              errors.ReleaseAndGetAddressOf());
  if (hr == E_UNEXPECTED) {
    REXLOG_ERROR("bf2native: the HLSL compiler crashed on {} ({} {}), source kept", name, entry,
                 target);
    std::error_code ec;
    std::filesystem::rename(pending, cache_dir_ / (std::string(name) + ".crashed.hlsl"), ec);
    return nullptr;
  }
  {
    std::error_code ec;
    std::filesystem::remove(pending, ec);
  }
  if (FAILED(hr)) {
    std::string msg = errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                           errors->GetBufferSize())
                             : std::string("?");
    if (msg.size() > 600) msg.resize(600);
    REXLOG_ERROR("bf2native: shader compile failed ({} {}): {}", entry, target, msg);
    std::ofstream dump(cache_dir_ / (std::string(name) + ".failed.hlsl"), std::ios::binary);
    dump << source;
    return nullptr;
  }
  std::ofstream out(path, std::ios::binary);
  out.write(static_cast<const char*>(blob->GetBufferPointer()),
            std::streamsize(blob->GetBufferSize()));
  return blob;
}

// --- Shaders -----------------------------------------------------------------------------------

bool Renderer::ReadUcode(uint32_t object, bool pixel, std::vector<uint32_t>& out) {
  PerfScope perf(5);
  if (!object) return false;
  uint32_t ptr = LoadV(object + (pixel ? 0x0C : 0x28));
  uint32_t bytes = LoadV(object + (pixel ? 0x3C : 0x258));
  if (!ptr || !bytes || bytes > 0x10000 || (bytes & 3)) return false;
  const uint8_t* src = ptr < kPhysicalSize ? Mem()->TranslatePhysical(ptr) : Virt(ptr);
  if (!pixel) {
    // The flush may have loaded a patched copy instead (see ImmediateVertexShader).
    const auto& imm = LoadedImmediateVertexShader();
    if (imm.shader == object && imm.code.size() == bytes) {
      uint32_t decl = LoadV(imm.device + dev::kVertexDeclaration);
      if (decl == imm.declaration && LoadV(decl + 32) == imm.declaration_id) src = imm.code.data();
    }
  }
  out.resize(bytes / 4);
  for (uint32_t i = 0; i < bytes / 4; ++i) out[i] = Be32(src + i * 4);
  return true;
}

VsEntry* Renderer::GetVertexShader(uint32_t object) {
  static thread_local std::vector<uint32_t> ucode;
  if (!ReadUcode(object, false, ucode)) return nullptr;
  uint64_t key = XXH3_64bits(ucode.data(), ucode.size() * 4);
  auto it = vs_cache_.find(key);
  if (it != vs_cache_.end()) return it->second->vs ? it->second.get() : nullptr;
  auto e = std::make_unique<VsEntry>();
  e->key = key;
  std::string hlsl = TranslateXenosShader(ucode.data(), uint32_t(ucode.size()), false, {}, e->info);
  DumpHlsl("vs", object, hlsl);
  if (auto blob = Compile(hlsl, "main", "vs_5_0")) {
    dev_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &e->vs);
  }
  VsEntry* r = e->vs ? e.get() : nullptr;
  vs_cache_[key] = std::move(e);
  return r;
}

PsEntry* Renderer::GetPixelShader(uint32_t object, uint32_t param_gen_reg) {
  static thread_local std::vector<uint32_t> ucode;
  if (!ReadUcode(object, true, ucode)) return nullptr;
  uint64_t key = XXH3_64bits(ucode.data(), ucode.size() * 4) ^ (uint64_t(param_gen_reg) << 56);
  auto it = ps_cache_.find(key);
  if (it != ps_cache_.end()) return it->second->ps ? it->second.get() : nullptr;
  auto e = std::make_unique<PsEntry>();
  e->key = key;
  TranslateOptions opt;
  opt.ps_param_gen_reg = param_gen_reg;
  std::string hlsl = TranslateXenosShader(ucode.data(), uint32_t(ucode.size()), true, opt, e->info);
  DumpHlsl("ps", object, hlsl);
  if (auto blob = Compile(hlsl, "main", "ps_5_0")) {
    dev_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &e->ps);
  }
  PsEntry* r = e->ps ? e.get() : nullptr;
  ps_cache_[key] = std::move(e);
  return r;
}

// --- Render targets ----------------------------------------------------------------------------

struct RtFormat {
  DXGI_FORMAT typeless, rtv, srv;
};

RtFormat ColorFormat(uint32_t f) {
  switch (f) {
    case 0:  // 8_8_8_8
      return {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
    case 1:  // 8_8_8_8_GAMMA: blend in linear space, sample the encoded values
      return {DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
              DXGI_FORMAT_R8G8B8A8_UNORM};
    case 2:
    case 10:
      return {DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
              DXGI_FORMAT_R10G10B10A2_UNORM};
    case 4:
    case 6:
      return {DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16_FLOAT};
    case 14:
      return {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT};
    case 15:
      return {DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32_FLOAT};
    default:  // 2_10_10_10_FLOAT, 16_16_16_16(_FLOAT), ...
      return {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
              DXGI_FORMAT_R16G16B16A16_FLOAT};
  }
}

// EDRAM formats that are different views of the same bits share one host target (games render
// e.g. 2_10_10_10_FLOAT and 2_10_10_10_FLOAT_AS_16_16_16_16 into the same surface).
uint32_t CanonicalColorFormat(uint32_t f) {
  switch (f) {
    case 1: return 0;
    case 10: return 2;
    case 12: return 3;
    default: return f;
  }
}

HostRT* Renderer::GetRT(uint32_t base, uint32_t format, uint32_t w, uint32_t h, bool depth) {
  if (!depth) format = CanonicalColorFormat(format);
  w = std::clamp(w, 1u, 8192u);
  h = std::clamp(h, 1u, 8192u);
  uint64_t key = uint64_t(base & 0xFFF) | (uint64_t(format & 0xF) << 12) |
                 (uint64_t(depth) << 16) | (uint64_t(w) << 17) | (uint64_t(h) << 31);
  auto it = rts_.find(key);
  if (it != rts_.end() && it->second.scale == ScaleFor(w)) return &it->second;
  HostRT rt;
  rt.width = w;
  rt.height = h;
  rt.guest_format = format;
  rt.depth = depth;
  D3D11_TEXTURE2D_DESC d{};
  rt.scale = ScaleFor(w);
  d.Width = ScaledSize(w, rt.scale);
  d.Height = ScaledSize(h, rt.scale);
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (depth) {
    d.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    d.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    rt.format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  } else {
    RtFormat f = ColorFormat(format);
    d.Format = f.typeless;
    d.BindFlags |= D3D11_BIND_RENDER_TARGET;
    rt.format = f.rtv;
  }
  if (FAILED(dev_->CreateTexture2D(&d, nullptr, &rt.tex))) return nullptr;
  if (depth) {
    D3D11_DEPTH_STENCIL_VIEW_DESC v{};
    v.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    v.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dev_->CreateDepthStencilView(rt.tex.Get(), &v, &rt.dsv);
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};
    s.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    s.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(rt.tex.Get(), &s, &rt.srv);
    ctx_->ClearDepthStencilView(rt.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
  } else {
    RtFormat f = ColorFormat(format);
    D3D11_RENDER_TARGET_VIEW_DESC v{};
    v.Format = f.rtv;
    v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    dev_->CreateRenderTargetView(rt.tex.Get(), &v, &rt.rtv);
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};
    s.Format = f.srv;
    s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    s.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(rt.tex.Get(), &s, &rt.srv);
    const float zero[4] = {0, 0, 0, 0};
    ctx_->ClearRenderTargetView(rt.rtv.Get(), zero);
  }
  REXLOG_INFO("bf2native: {} target base {} fmt {} {}x{}", depth ? "depth" : "color", base & 0xFFF,
              format, w, h);
  return &(rts_[key] = std::move(rt));
}

// Size of a guest D3D surface object (+24: width-1 bits 0-12, height-1 bits 13-25).
bool SurfaceSize(uint32_t surface, uint32_t& w, uint32_t& h) {
  if (!surface) return false;
  uint32_t v = LoadV(surface + 24);
  w = (v & 0x1FFF) + 1;
  h = ((v >> 13) & 0x1FFF) + 1;
  return true;
}

bool Renderer::CurrentTargets(uint32_t device, Targets& t, bool want_color) {
  t = Targets();
  uint32_t surfaces[4], depth_surface = LoadV(device + dev::kDepthSurface);
  for (uint32_t i = 0; i < 4; ++i) surfaces[i] = LoadV(device + dev::kColorSurfaces + i * 4);
  uint32_t w = 0, h = 0;
  bool have = false;
  for (uint32_t i = 0; i < 4 && !have; ++i) have = SurfaceSize(surfaces[i], w, h);
  if (!have) have = SurfaceSize(depth_surface, w, h);
  if (!have) return false;
  t.width = std::clamp(w, 1u, 8192u);
  t.height = std::clamp(h, 1u, 8192u);
  t.scale = ScaleFor(t.width);
  uint32_t edram_mode = Reg(device, 0x2208) & 7;
  static const uint32_t kInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
  if (want_color && edram_mode != 5) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (!surfaces[i]) continue;
      uint32_t info = Reg(device, kInfo[i]);
      uint32_t format = (info >> 16) & 0xF;
      t.color[i] = GetRT(info & 0xFFF, format, t.width, t.height, false);
      t.gamma[i] = format == 1;
      if (t.color[i] && t.gamma[i] && !t.color[i]->rtv_gamma) {
        D3D11_RENDER_TARGET_VIEW_DESC v{};
        v.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        dev_->CreateRenderTargetView(t.color[i]->tex.Get(), &v, &t.color[i]->rtv_gamma);
      }
    }
  }
  if (depth_surface) {
    uint32_t info = Reg(device, 0x2002);
    t.depth = GetRT(info & 0xFFF, (info >> 16) & 1, t.width, t.height, true);
  }
  return true;
}

void Renderer::BindTargets(const Targets& t, uint32_t color_mask) {
  ID3D11RenderTargetView* rtvs[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    if (t.color[i] && (color_mask & (1u << i))) {
      rtvs[i] = (t.gamma[i] && t.color[i]->rtv_gamma ? t.color[i]->rtv_gamma : t.color[i]->rtv).Get();
    }
  }
  ctx_->OMSetRenderTargets(4, rtvs, t.depth ? t.depth->dsv.Get() : nullptr);
}

// --- Vertex memory mirror ----------------------------------------------------------------------

// Vertex data reaches the mirror in two steps, like the console GPU, which reads guest memory
// when it executes the frame rather than when the draw is issued:
// - At draw time, chunks that changed are captured (copied into the pool, and a pool -> mirror
//   copy is recorded before the draw). Capturing per draw keeps buffers the game rewrites within
//   one frame (the D3D library's UP ring, reused at once since fences complete immediately here)
//   correct for each draw.
// - When the frame executes (LateSyncMirror), chunks the game wrote after the draws that use them
//   are refreshed: a single capture gets the final contents, an uncaptured chunk is uploaded
//   before the frame runs.
void Renderer::SyncMirror(uint32_t address, uint32_t size, bool force) {
  PerfScope perf(1);
  if (!size) return;
  address &= 0x1FFFFFFFu;
  size = std::min(size, kPhysicalSize - address);
  const uint8_t* phys = Mem()->physical_membase();
  uint32_t first = address >> kChunkShift, last = (address + size - 1) >> kChunkShift;
  uint32_t run_start = UINT32_MAX;
  auto flush = [&](uint32_t end_chunk) {
    if (run_start == UINT32_MAX) return;
    uint32_t bytes = (end_chunk - run_start) << kChunkShift;
    const uint8_t* src = phys + (run_start << kChunkShift);
    if (pool_ && pool_used_ + bytes <= kPoolSize) {
      std::memcpy(pool_cpu_.data() + pool_used_, src, bytes);
      D3D11_BOX box{pool_used_, 0, 0, pool_used_ + bytes, 1, 1};
      ctx_->CopySubresourceRegion(mirror_.Get(), 0, run_start << kChunkShift, 0, 0, pool_.Get(), 0, &box);
      for (uint32_t c = run_start; c < end_chunk; ++c) {
        ChunkState& s = chunks_[c];
        s.pool_offset = ++s.captures == 1 ? pool_used_ + ((c - run_start) << kChunkShift) : UINT32_MAX;
      }
      pool_used_ += bytes;
    } else {
      D3D11_BOX box{run_start << kChunkShift, 0, 0, end_chunk << kChunkShift, 1, 1};
      if (buggy_deferred_update_) src -= box.left;  // runtime emulation offsets pSrcData by the box
      ctx_->UpdateSubresource(mirror_.Get(), 0, &box, src, 0, 0);
      for (uint32_t c = run_start; c < end_chunk; ++c) {
        chunks_[c].captures += 2;  // not patchable later
        chunks_[c].pool_offset = UINT32_MAX;
      }
    }
    run_start = UINT32_MAX;
  };
  for (uint32_t c = first; c <= last; ++c) {
    ChunkState& s = chunks_[c];
    if (s.used_frame != frame_) {
      s.used_frame = frame_;
      s.captures = 0;
      s.pool_offset = UINT32_MAX;
      frame_chunks_.push_back(c);
    }
    bool check = force || !s.valid || s.checked_frame != frame_ ||
                 (frame_ - s.changed_frame < 3 && s.checked_draw != draw_serial_);
    bool dirty = false;
    if (check) {
      uint64_t h = XXH3_64bits(phys + (size_t(c) << kChunkShift), size_t(1) << kChunkShift);
      dirty = !s.valid || h != s.hash;
      if (dirty && s.valid) s.changed_frame = frame_;
      s.hash = h;
      s.valid = true;
      s.checked_frame = frame_;
      s.checked_draw = draw_serial_;
    }
    if (dirty) {
      if (run_start == UINT32_MAX) run_start = c;
    } else {
      flush(c);
    }
  }
  flush(last + 1);
}

void Renderer::LateSyncMirror() {
  PerfScope perf(3);
  static const bool no_late = std::getenv("BF2_NO_LATE_SYNC") != nullptr;
  const uint8_t* phys = Mem()->physical_membase();
  uint32_t late = 0;
  for (uint32_t c : frame_chunks_) {
    if (no_late) continue;
    ChunkState& s = chunks_[c];
    if (s.used_frame != frame_ || s.captures > 1 || frame_ - s.changed_frame >= kLateSyncFrames) continue;
    const uint8_t* src = phys + (size_t(c) << kChunkShift);
    uint64_t h = XXH3_64bits(src, size_t(1) << kChunkShift);
    if (h == s.hash) continue;
    if (frame_ == trace_frame_) BF2_TRACE("bf2trace: late mirror {:08X} captures {}", c << kChunkShift, s.captures);
    if (s.captures == 1 && s.pool_offset != UINT32_MAX) {
      std::memcpy(pool_cpu_.data() + s.pool_offset, src, size_t(1) << kChunkShift);
    } else {
      D3D11_BOX box{c << kChunkShift, 0, 0, (c + 1) << kChunkShift, 1, 1};
      imm_->UpdateSubresource(mirror_.Get(), 0, &box, src, 0, 0);
    }
    s.hash = h;
    s.changed_frame = frame_;
    ++late;
  }
  if (pool_used_) {
    D3D11_BOX box{0, 0, 0, pool_used_, 1, 1};
    imm_->UpdateSubresource(pool_.Get(), 0, &box, pool_cpu_.data(), 0, 0);
  }
  if (late && frame_ == trace_frame_) BF2_TRACE("bf2trace: {} mirror chunks written after their draws", late);
  pool_used_ = 0;
  frame_chunks_.clear();
}

// --- Textures ----------------------------------------------------------------------------------

DXGI_FORMAT ResolveFormat(uint32_t tf) {
  switch (tf) {
    case 7:
    case 54:
      return DXGI_FORMAT_R10G10B10A2_UNORM;
    case 22:
    case 23:
    case 33:
    case 36:
      return DXGI_FORMAT_R32_FLOAT;
    case 27:  // 16_EXPAND: float16 in the texture unit
    case 30:
      return DXGI_FORMAT_R16_FLOAT;
    case 28:
    case 31:
      return DXGI_FORMAT_R16G16_FLOAT;
    case 29:
    case 32:
      return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case 13:
    case 25:
      return DXGI_FORMAT_R16G16_UNORM;
    case 21:
    case 26:
      return DXGI_FORMAT_R16G16B16A16_UNORM;
    case 34:
    case 37:
      return DXGI_FORMAT_R32G32_FLOAT;
    case 35:
    case 38:
      return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case 2:
      return DXGI_FORMAT_R8_UNORM;
    case 10:
      return DXGI_FORMAT_R8G8_UNORM;
    default:
      return DXGI_FORMAT_R8G8B8A8_UNORM;
  }
}

// Cheap signature of the guest memory behind a resolve destination: detects the address being
// reused for a CPU-written texture (the resolve itself never writes guest memory).
uint64_t Renderer::GuestSignature(const TexFetch& f) {
  const uint8_t* phys = Mem()->physical_membase();
  uint32_t base = f.base_address() & 0x1FFFFFFFu;
  // Only the texture's own (tiled, 32x32-aligned) extent at 4 bytes per texel: neighbouring
  // memory can be CPU data that changes every frame.
  uint32_t extent = ((f.width() + 31) & ~31u) * ((f.height() + 31) & ~31u) * 4;
  uint32_t n = std::min<uint32_t>({64u * 1024, extent, kPhysicalSize - base});
  return XXH3_64bits(phys + base, n);
}

Resolved* Renderer::GetResolved(const TexFetch& f, float scale) {
  uint32_t base = f.base_address() & 0x1FFFFFFFu;
  uint32_t w = f.width(), h = f.height();
  uint32_t dim = f.dimension();
  bool cube = dim == 3;
  uint32_t slices = cube ? 6 : (dim == 1 ? f.depth() : 1);
  auto it = resolved_.find(base);
  if (it != resolved_.end()) {
    Resolved& r = it->second;
    if (r.width == w && r.height == h && r.slices == slices && r.cube == cube &&
        r.guest_format == f.format() && r.scale == scale) {
      return &r;
    }
    resolved_.erase(it);
  }
  Resolved r;
  r.width = w;
  r.height = h;
  r.slices = slices;
  r.cube = cube;
  r.guest_format = f.format();
  r.scale = scale;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = ScaledSize(w, scale);
  d.Height = ScaledSize(h, scale);
  d.MipLevels = 1;
  d.ArraySize = slices;
  d.Format = ResolveFormat(f.format());
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  d.MiscFlags = cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
  if (FAILED(dev_->CreateTexture2D(&d, nullptr, &r.tex))) return nullptr;
  D3D11_SHADER_RESOURCE_VIEW_DESC s{};
  s.Format = d.Format;
  if (cube) {
    s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
    s.TextureCube.MipLevels = 1;
  } else {
    s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    s.Texture2DArray.MipLevels = 1;
    s.Texture2DArray.ArraySize = slices;
  }
  dev_->CreateShaderResourceView(r.tex.Get(), &s, &r.srv);
  if (!cube) {
    D3D11_SHADER_RESOURCE_VIEW_DESC s0{};
    s0.Format = d.Format;
    s0.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    s0.Texture2D.MipLevels = 1;
    if (slices == 1) dev_->CreateShaderResourceView(r.tex.Get(), &s0, &r.srv_slice0);
  }
  for (uint32_t i = 0; i < slices; ++i) {
    D3D11_RENDER_TARGET_VIEW_DESC v{};
    v.Format = d.Format;
    v.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    v.Texture2DArray.FirstArraySlice = i;
    v.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11RenderTargetView> rtv;
    dev_->CreateRenderTargetView(r.tex.Get(), &v, &rtv);
    const float black[4] = {0, 0, 0, 1};
    if (rtv) ctx_->ClearRenderTargetView(rtv.Get(), black);
    r.rtvs.push_back(rtv);
  }
  REXLOG_INFO("bf2native: resolve target {:08X} fmt {} {}x{}x{}", base, f.format(), w, h, slices);
  return &(resolved_[base] = std::move(r));
}

ID3D11ShaderResourceView* Renderer::GetResolvedSrv(const TexFetch& f, TexDim want) {
  auto it = resolved_.find(f.base_address() & 0x1FFFFFFFu);
  if (it == resolved_.end()) return nullptr;
  Resolved& r = it->second;
  if (r.checked_frame != frame_) {
    r.checked_frame = frame_;
    if (GuestSignature(f) != r.signature) {  // the CPU put something else there
      static int logged = 0;
      if (logged++ < 20) {
        REXLOG_INFO("bf2native: resolve target {:08X} overwritten by the CPU, dropped", it->first);
      }
      resolved_.erase(it);
      return nullptr;
    }
  }
  bool is_cube = want == TexDim::kCube;
  if (is_cube != r.cube || want == TexDim::k3D) return nullptr;
  return r.srv.Get();
}

ID3D11ShaderResourceView* Renderer::GetTexture(const TexFetch& f, TexDim want) {
  if (f.type() != 2 || !f.base_address()) return nullptr;
  if (auto* srv = GetResolvedSrv(f, want)) return srv;
  uint32_t dim = f.dimension();
  TexDim have = dim == 2 ? TexDim::k3D : dim == 3 ? TexDim::kCube : TexDim::k2D;
  TexDim want_class = want == TexDim::k1D ? TexDim::k2D : want;
  if (have != want_class) {
    static int logged = 0;
    if (logged++ < 20) REXLOG_WARN("bf2native: texture {:08X} dim {} bound as {}", f.base_address(), dim, int(want));
    return nullptr;
  }

  // Identity: everything except sampler state (clamp, filters, swizzle, bias, border).
  struct Key {
    uint32_t d0, d1, d2, d4, d5;
  } key{f.d[0] & ~(0x1FFu << 10), f.d[1], f.d[2], f.d[4] & (0xFFu << 2), f.d[5] & ~3u};
  uint64_t k = HashOf(key);
  HostTex& t = textures_[k];
  if (t.srv) {  // contents are refreshed at Swap (RefreshTextures)
    if (t.checked_frame != frame_) {
      t.checked_frame = frame_;
      t.fetch = f;
      frame_textures_.push_back(k);
    }
    return t.srv.Get();
  }
  if (t.checked_frame == frame_) return nullptr;
  t.checked_frame = frame_;
  const uint8_t* phys = Mem()->physical_membase();
  uint64_t h = HashTextureMemory(phys, f);
  if (t.failed && h == t.mem_hash) return nullptr;
  t.mem_hash = h;
  t.failed = true;
  t.srv.Reset();
  t.res.Reset();

  auto fail = [&](const char* why) -> ID3D11ShaderResourceView* {
    static int logged = 0;
    if (logged < 60) {
      ++logged;
      REXLOG_WARN("bf2native: texture {:08X} fmt {} {}x{}x{} dim {} tiled {}: {}", f.base_address(),
                  f.format(), f.width(), f.height(), f.depth(), f.dimension(), f.tiled(), why);
    }
    return nullptr;
  };
  HostTexture ht;
  if (!DecodeTexture(phys, f, ht)) return fail("decode");
  // Levels that could not be located come back empty: keep the leading complete ones.
  uint32_t array = ht.is_3d ? 1 : ht.depth;
  uint32_t levels = ht.levels;
  for (uint32_t l = 0; l < ht.levels; ++l) {
    if (ht.subs[l].data.empty()) {
      levels = l;
      break;
    }
  }
  if (!levels) return fail("no levels");
  std::vector<D3D11_SUBRESOURCE_DATA> init;
  for (uint32_t s = 0; s < array; ++s) {
    for (uint32_t l = 0; l < levels; ++l) {
      const auto& sub = ht.subs[size_t(s) * ht.levels + l];
      if (sub.data.empty()) return fail("missing level");
      init.push_back({sub.data.data(), sub.row_pitch, sub.slice_pitch});
    }
  }
  if (dump_hlsl_ && ht.is_3d && ht.format == DXGI_FORMAT_R8G8B8A8_UNORM) {
    // Debug aid: the volume's z slices side by side.
    const auto& sub = ht.subs[0];
    uint32_t w = ht.width, hh = ht.height, dd = ht.depth;
    std::vector<uint8_t> strip(size_t(w) * dd * hh * 4);
    for (uint32_t z = 0; z < dd; ++z)
      for (uint32_t y = 0; y < hh; ++y)
        std::memcpy(&strip[(size_t(y) * w * dd + size_t(z) * w) * 4],
                    &sub.data[size_t(z) * sub.slice_pitch + size_t(y) * sub.row_pitch], w * 4);
    char name[48];
    snprintf(name, sizeof(name), "tex3d_%08X.bmp", f.base_address());
    WriteBmp(name, strip.data(), w * dd * 4, w * dd, hh);
  }
  bool bc = ht.format >= DXGI_FORMAT_BC1_TYPELESS && ht.format <= DXGI_FORMAT_BC5_SNORM;
  uint32_t w = ht.width, hh = ht.height;
  if (bc) {  // D3D11 requires block-aligned top levels
    w = (w + 3) & ~3u;
    hh = (hh + 3) & ~3u;
  }
  HRESULT hr;
  if (ht.is_3d) {
    D3D11_TEXTURE3D_DESC d{};
    d.Width = w;
    d.Height = hh;
    d.Depth = ht.depth;
    d.MipLevels = levels;
    d.Format = ht.format;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture3D> tex;
    hr = dev_->CreateTexture3D(&d, init.data(), &tex);
    if (FAILED(hr)) return fail("CreateTexture3D");
    t.res = tex;
    dev_->CreateShaderResourceView(tex.Get(), nullptr, &t.srv);
  } else {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w;
    d.Height = hh;
    d.MipLevels = levels;
    d.ArraySize = array;
    d.Format = ht.format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = ht.is_cube ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
    if (ht.is_cube && array != 6) return fail("cube faces");
    ComPtr<ID3D11Texture2D> tex;
    hr = dev_->CreateTexture2D(&d, init.data(), &tex);
    if (FAILED(hr)) return fail("CreateTexture2D");
    t.res = tex;
    D3D11_SHADER_RESOURCE_VIEW_DESC s{};
    s.Format = ht.format;
    if (ht.is_cube) {
      s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
      s.TextureCube.MipLevels = levels;
    } else {
      s.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
      s.Texture2DArray.MipLevels = levels;
      s.Texture2DArray.ArraySize = array;
    }
    dev_->CreateShaderResourceView(tex.Get(), &s, &t.srv);
  }
  t.failed = !t.srv;
  t.dim = have;
  t.levels = levels;
  t.fetch = f;
  if (t.srv) frame_textures_.push_back(k);
  return t.srv.Get();
}

ID3D11SamplerState* Renderer::GetSampler(const TexFetch& f, const TextureBinding& b) {
  uint32_t mag = b.mag != 0xFF ? b.mag : f.mag_filter();
  uint32_t min = b.min != 0xFF ? b.min : f.min_filter();
  uint32_t mip = b.mip != 0xFF ? b.mip : f.mip_filter();
  uint32_t aniso = b.aniso != 0xFF ? b.aniso : f.aniso_filter();
  // PC settings: anisotropic filtering forced on filtered (linear mip) fetches.
  if (int forced = pcs::Anisotropy(); forced > 1 && min == 1 && mag == 1 && mip == 1)
    aniso = std::max<uint32_t>(aniso >= 2 && aniso <= 5 ? aniso : 0, uint32_t(std::countr_zero(unsigned(forced))) + 1);
  D3D11_SAMPLER_DESC d;
  ZeroMemory(&d, sizeof(d));
  if (aniso >= 2 && aniso <= 5) {
    d.Filter = D3D11_FILTER_ANISOTROPIC;
    d.MaxAnisotropy = 1u << (aniso - 1);
  } else {
    d.Filter = D3D11_FILTER(((min == 1) << 4) | ((mag == 1) << 2) | (mip == 1));
    d.MaxAnisotropy = 1;
  }
  static const D3D11_TEXTURE_ADDRESS_MODE kAddr[8] = {
      D3D11_TEXTURE_ADDRESS_WRAP,        D3D11_TEXTURE_ADDRESS_MIRROR,
      D3D11_TEXTURE_ADDRESS_CLAMP,       D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
      D3D11_TEXTURE_ADDRESS_CLAMP,       D3D11_TEXTURE_ADDRESS_MIRROR_ONCE,
      D3D11_TEXTURE_ADDRESS_BORDER,      D3D11_TEXTURE_ADDRESS_MIRROR_ONCE};
  d.AddressU = kAddr[f.clamp(0)];
  d.AddressV = kAddr[f.clamp(1)];
  d.AddressW = kAddr[f.clamp(2)];
  d.MipLODBias = std::clamp(f.lod_bias(), -16.0f, 15.99f);
  d.MinLOD = float(f.mip_min_level());
  d.MaxLOD = mip == 2 ? d.MinLOD : std::max(d.MinLOD, float(f.mip_max_level()));
  d.ComparisonFunc = D3D11_COMPARISON_NEVER;
  float border = f.border_color() == 2 ? 1.0f : 0.0f;
  d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = border;
  d.BorderColor[3] = f.border_color() == 0 ? 0.0f : 1.0f;
  return GetSamplerState(d);
}

// --- State object caches -----------------------------------------------------------------------

ID3D11BlendState* Renderer::GetBlend(const D3D11_BLEND_DESC& d) {
  auto& e = blend_cache_[HashOf(d)];
  if (!e) dev_->CreateBlendState(&d, &e);
  return e.Get();
}
ID3D11DepthStencilState* Renderer::GetDepthStencil(const D3D11_DEPTH_STENCIL_DESC& d) {
  auto& e = ds_cache_[HashOf(d)];
  if (!e) dev_->CreateDepthStencilState(&d, &e);
  return e.Get();
}
ID3D11RasterizerState* Renderer::GetRasterizer(const D3D11_RASTERIZER_DESC& d) {
  auto& e = rs_cache_[HashOf(d)];
  if (!e) dev_->CreateRasterizerState(&d, &e);
  return e.Get();
}
ID3D11SamplerState* Renderer::GetSamplerState(const D3D11_SAMPLER_DESC& d) {
  auto& e = sampler_cache_[HashOf(d)];
  if (!e) dev_->CreateSamplerState(&d, &e);
  return e.Get();
}

void Renderer::UpdateBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* buffer, const void* data,
                            size_t size) {
  PerfScope perf(4);
  ConstantSnapshot* snapshot = nullptr;
  if (ctx == ctx_.Get()) {
    if (buffer == vs_consts_.Get()) snapshot = &constant_snapshots_[0];
    else if (buffer == ps_consts_.Get()) snapshot = &constant_snapshots_[1];
    else if (buffer == state_cb_.Get()) snapshot = &constant_snapshots_[2];
  }
  if (snapshot && snapshot->valid && snapshot->bytes.size() == size &&
      std::memcmp(snapshot->bytes.data(), data, size) == 0) return;
  D3D11_MAPPED_SUBRESOURCE m;
  if (SUCCEEDED(ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
    std::memcpy(m.pData, data, size);
    ctx->Unmap(buffer, 0);
    if (snapshot) {
      snapshot->bytes.resize(size);
      std::memcpy(snapshot->bytes.data(), data, size);
      snapshot->valid = true;
    }
  } else if (snapshot) {
    snapshot->valid = false;  // failed uploads must be retried
  }
}

void Renderer::SetUtil(const UtilConsts& u) { UpdateBuffer(util_cb_.Get(), &u, sizeof(u)); }

// --- Translation of Xenos render state ---------------------------------------------------------

D3D11_BLEND BlendFactor(uint32_t f, bool alpha) {
  switch (f) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 4: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 5: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 6: return D3D11_BLEND_SRC_ALPHA;
    case 7: return D3D11_BLEND_INV_SRC_ALPHA;
    case 8: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 9: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 10: return D3D11_BLEND_DEST_ALPHA;
    case 11: return D3D11_BLEND_INV_DEST_ALPHA;
    case 12:
    case 14: return D3D11_BLEND_BLEND_FACTOR;
    case 13:
    case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 16: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    default: return D3D11_BLEND_ZERO;
  }
}
D3D11_BLEND_OP BlendOp(uint32_t op) {
  switch (op) {
    case 1: return D3D11_BLEND_OP_SUBTRACT;
    case 2: return D3D11_BLEND_OP_MIN;
    case 3: return D3D11_BLEND_OP_MAX;
    case 4: return D3D11_BLEND_OP_REV_SUBTRACT;
    default: return D3D11_BLEND_OP_ADD;
  }
}

// Expands guest vertex indices (UINT32_MAX = primitive restart) into a host list topology.
bool ConvertPrimitives(uint32_t prim, const std::vector<uint32_t>& in, std::vector<uint32_t>& out,
                       Kind& kind) {
  out.clear();
  switch (prim) {
    case 0x01: kind = Kind::kPoints; break;
    case 0x02: case 0x03: case 0x0C: kind = Kind::kLines; break;
    case 0x04: case 0x05: case 0x06: case 0x07: case 0x0D: case 0x0E: case 0x0F:
      kind = Kind::kTris;
      break;
    case 0x08: kind = Kind::kRects; break;
    default: return false;
  }
  size_t seg_start = 0;
  for (size_t i = 0; i <= in.size(); ++i) {
    if (i < in.size() && in[i] != UINT32_MAX) continue;
    const uint32_t* s = in.data() + seg_start;
    size_t n = i - seg_start;
    seg_start = i + 1;
    switch (prim) {
      case 0x01:
        out.insert(out.end(), s, s + n);
        break;
      case 0x02:
        out.insert(out.end(), s, s + (n & ~size_t(1)));
        break;
      case 0x03:
      case 0x0C:
        for (size_t k = 0; k + 1 < n; ++k) {
          out.push_back(s[k]);
          out.push_back(s[k + 1]);
        }
        if (prim == 0x0C && n > 2) {
          out.push_back(s[n - 1]);
          out.push_back(s[0]);
        }
        break;
      case 0x04:
      case 0x07:
      case 0x08:
        out.insert(out.end(), s, s + n / 3 * 3);
        break;
      case 0x05:
      case 0x0F:
        for (size_t k = 1; k + 1 < n; ++k) {
          out.push_back(s[0]);
          out.push_back(s[k]);
          out.push_back(s[k + 1]);
        }
        break;
      case 0x06:
        for (size_t k = 0; k + 2 < n; ++k) {
          if (k & 1) {
            out.push_back(s[k + 1]);
            out.push_back(s[k]);
          } else {
            out.push_back(s[k]);
            out.push_back(s[k + 1]);
          }
          out.push_back(s[k + 2]);
        }
        break;
      case 0x0D:
        for (size_t k = 0; k + 3 < n; k += 4) {
          const uint32_t q[6] = {s[k], s[k + 1], s[k + 2], s[k], s[k + 2], s[k + 3]};
          out.insert(out.end(), q, q + 6);
        }
        break;
      case 0x0E:
        for (size_t k = 0; k + 3 < n; k += 2) {
          const uint32_t q[6] = {s[k], s[k + 1], s[k + 3], s[k], s[k + 3], s[k + 2]};
          out.insert(out.end(), q, q + 6);
        }
        break;
    }
  }
  return !out.empty();
}

// --- Draw --------------------------------------------------------------------------------------

void Renderer::Draw(uint32_t device, uint32_t prim, std::vector<uint32_t>& vids,
                    const UpDraw* up) {
  PerfScope perf(0);
  PerfScope prepare_perf(10);
  ++draw_serial_;
  ++frame_draws_;
  auto skip = [&](const char* why) {
    ++frame_skipped_;
    static int logged = 0;
    if (logged < 40) {
      ++logged;
      REXLOG_WARN("bf2native: draw skipped ({}), prim {}", why, prim);
    }
  };

  static thread_local std::vector<uint32_t> indices;
  Kind kind;
  // DXGI R32 indices use UINT32_MAX as strip cut, matching the restart markers decoded above.
  // Keep strips on the GPU instead of expanding every triangle and uploading 3x the indices.
  const bool native_strip = prim == 0x06 || prim == 0x03;
  if (native_strip) kind = prim == 0x06 ? Kind::kTris : Kind::kLines;
  else if (!ConvertPrimitives(prim, vids, indices, kind)) return skip("primitive");
  const auto& host_indices = native_strip ? vids : indices;
  if (host_indices.size() > kIndexRingCount) return skip("too many indices");

  uint32_t mode = Reg(device, 0x2205);
  if (kind == Kind::kTris && (mode & 3) == 3) return;  // both faces culled

  VsEntry* vs = GetVertexShader(LoadV(device + dev::kVertexShader));
  if (!vs) return skip("vertex shader");
  uint32_t program_cntl = Reg(device, 0x2180);
  uint32_t param_gen = (program_cntl >> 18) & 1 ? (Reg(device, 0x2181) >> 8) & 0xFF : ~0u;
  uint32_t ps_object = LoadV(device + dev::kPixelShader);
  PsEntry* ps = ps_object ? GetPixelShader(ps_object, param_gen) : nullptr;
  if (ps_object && !ps) return skip("pixel shader");

  // BF2_SKIP_SHADER=<hash>[,<hash>...]: debugging aid, drops draws using those shaders (keys as
  // printed by the trace).
  // BF2_SKIP_TOGGLE=1: only every other 90-frame period, to compare frames from one run.
  if (!skip_shaders_.empty() && (!skip_toggle_ || ((frame_ / 90) & 1))) {
    for (uint64_t k : skip_shaders_)
      if (k == vs->key || (ps && k == ps->key)) return;
  }

  Targets t;
  if (!CurrentTargets(device, t)) return skip("no render target");

  // --- Constants and state block.
  static XState st;
  uint8_t* d = Virt(device);
  for (int i = 0; i < 8; ++i) st.xb[i] = Be32(d + dev::kBoolLoopConstants + i * 4);
  for (int i = 0; i < 32; ++i) st.xl[i] = Be32(d + dev::kBoolLoopConstants + 32 + i * 4);
  for (int i = 0; i < 192; ++i) st.xf[i] = Be32(d + dev::kFetchConstants + i * 4);
  if (up) {
    // UP draws read their vertices from the command buffer through vertex fetch constant 95.
    uint32_t phys = PhysOf(up->vertices);
    uint32_t bytes = up->vertex_count * up->stride;
    st.xf[190] = (phys & ~3u) | 3u;
    st.xf[191] = (((bytes + 3) / 4) << 2) | 2u;
  }
  for (uint32_t k = 0; k < 96; ++k) {
    if ((st.xf[k / 3 * 6] & 3) == 2) continue;  // slot holds a texture fetch constant
    uint32_t& d0 = st.xf[k * 2];
    if ((d0 & 3) == 3 && (d0 & ~3u) >= kPhysicalSize) d0 = (PhysOf(d0 & ~3u) & 0x1FFFFFFCu) | 3u;
  }

  uint32_t vte = Reg(device, 0x2206);
  float sx = (vte & 1) ? RegF(device, 0x210F) : 1.0f;
  float ox = (vte & 2) ? RegF(device, 0x2110) : 0.0f;
  float sy = (vte & 4) ? RegF(device, 0x2111) : 1.0f;
  float oy = (vte & 8) ? RegF(device, 0x2112) : 0.0f;
  float sz = (vte & 16) ? RegF(device, 0x2113) : 1.0f;
  float oz = (vte & 32) ? RegF(device, 0x2114) : 0.0f;
  float vx = 0, vy = 0, vw = float(t.width), vh = float(t.height);
  if ((vte & 1) && std::fabs(sx) >= 0.5f) {
    vw = 2.0f * std::fabs(sx);
    vx = ox - std::fabs(sx);
  }
  if ((vte & 4) && std::fabs(sy) >= 0.5f) {
    vh = 2.0f * std::fabs(sy);
    vy = oy - std::fabs(sy);
  }
  vw = std::min(vw, 16384.0f);
  vh = std::min(vh, 16384.0f);
  vx = std::clamp(vx, -16384.0f, 16383.0f);
  vy = std::clamp(vy, -16384.0f, 16383.0f);
  // Build floats in float storage, then copy their bits. Aliasing a uint32 array through a
  // float pointer is undefined in optimized C++ and may reorder/drop state writes.
  std::array<float, kSysFloat4Count * 4> sys{};
  sys[0] = sx; sys[1] = ox; sys[2] = sy; sys[3] = oy;
  sys[4] = sz; sys[5] = oz; sys[6] = 2.0f / vw; sys[7] = 2.0f / vh;
  sys[9] = (Reg(device, 0x2302) & 1) == 0 ? 0.5f : 0.0f;  // D3D9 pixel centers
  uint32_t colorcontrol = Reg(device, 0x2202);
  sys[13] = RegF(device, 0x210E);
  static const uint32_t kInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
  for (int i = 0; i < 4; ++i) {
    int32_t bias = int32_t(Reg(device, kInfo[i]) << 6) >> 26;
    sys[16 + i] = std::ldexp(1.0f, bias);
  }
  uint32_t point_size = Reg(device, 0x2280);
  sys[20] = vx;
  sys[21] = vy;
  sys[22] = float(point_size >> 16) * 2.0f / 16.0f;
  sys[23] = float(point_size & 0xFFFF) * 2.0f / 16.0f;
  sys[24] = 1.0f / t.scale;  // host pixels -> guest pixels (pixel position parameter)
  std::memcpy(st.xsys, sys.data(), sizeof(st.xsys));
  st.xsys[8] = ((vte >> 8) & 1) | (((vte >> 9) & 1) << 1) | (((vte >> 10) & 1) << 2);
  st.xsys[12] = (colorcontrol & 8) ? (colorcontrol & 7) : 7u;
  st.xsys[14] = kind == Kind::kPoints ? 2u : kind == Kind::kLines ? 4u : 1u;
  UpdateBuffer(state_cb_.Get(), &st, sizeof(st));

  static uint32_t consts[1024];
  for (int i = 0; i < 1024; ++i) consts[i] = Be32(d + dev::kVsConstants + i * 4);
  UpdateBuffer(vs_consts_.Get(), consts, 4096);
  if (ps) {
    for (int i = 0; i < 1024; ++i) consts[i] = Be32(d + dev::kPsConstants + i * 4);
    UpdateBuffer(ps_consts_.Get(), consts, 4096);
    if (frame_ == trace_frame_ && prim == 13) {
      for (int r : {0, 1, 2, 3, 20, 21, 22, 252, 253, 254, 255}) {
        BF2_TRACE("bf2trace:   psc[{}] {} {} {} {}", r, AsFloat(consts[r * 4]), AsFloat(consts[r * 4 + 1]),
                    AsFloat(consts[r * 4 + 2]), AsFloat(consts[r * 4 + 3]));
      }
    }
  }

  prepare_perf.Stop();
  // --- Vertex data.
  for (uint32_t k = 0; k < 96; ++k) {
    if (!((vs->info.vfetch_used[k >> 6] >> (k & 63)) & 1)) continue;
    uint32_t d0 = st.xf[k * 2], d1 = st.xf[k * 2 + 1];
    if ((d0 & 3) != 3) continue;
    uint32_t bytes = ((d1 >> 2) & 0xFFFFFF) * 4;
    SyncMirror(d0 & 0x1FFFFFFCu, std::min(bytes, 64u << 20), up && k == 95);
  }

  // --- Textures and samplers.
  auto bind_textures = [&](const ShaderInfo& info, bool pixel) {
    ID3D11ShaderResourceView* srvs[1 + kStackedSrvBase + 16] = {};
    uint32_t srv_count = 0;
    ID3D11SamplerState* samplers[16] = {};
    uint32_t n = std::min<uint32_t>(uint32_t(info.textures.size()), 16);
    for (uint32_t i = 0; i < n; ++i) {
      const TextureBinding& b = info.textures[i];
      TexFetch f;
      std::memcpy(f.d, &st.xf[b.slot * 6], 24);
      NormalizeFetch(f);
      ID3D11ShaderResourceView* srv = GetTexture(f, b.dim);
      if (b.dim == TexDim::k3D) {
        // 3D-or-stacked fetch: the shader picks the 3D or the 2D-array view at run time.
        ID3D11ShaderResourceView* stacked = GetTexture(f, TexDim::k2D);
        srvs[1 + kStackedSrvBase + i] = stacked ? stacked : black_2d_.Get();
        srv_count = std::max(srv_count, 2 + kStackedSrvBase + i);
        if (!srv && stacked) srv = black_3d_.Get();
      }
      if (!srv) {
        srv = b.dim == TexDim::k3D ? black_3d_.Get()
              : b.dim == TexDim::kCube ? black_cube_.Get()
                                       : black_2d_.Get();
      }
      srv_count = std::max(srv_count, 2 + i);
      if (frame_ == trace_frame_) {
        BF2_TRACE("bf2trace:   {} t{} slot {} dim {} -> fetch type {} base {:08X} fmt {} {}x{} fdim {} sign {:X} swz {:03X} {}",
                    pixel ? "ps" : "vs", i, b.slot, int(b.dim), f.type(), f.base_address(), f.format(),
                    f.width(), f.height(), f.dimension(), (f.d[0] >> 2) & 0xFF, f.swizzle(),
                    (srv == black_2d_.Get() || srv == black_3d_.Get() || srv == black_cube_.Get()) &&
                            (b.dim != TexDim::k3D || srvs[1 + kStackedSrvBase + i] == black_2d_.Get())
                        ? "FALLBACK"
                        : "ok");
      }
      srvs[i + 1] = srv;
      samplers[i] = f.type() == 2 ? GetSampler(f, b) : linear_clamp_.Get();
      if (frame_ == trace_frame_ && samplers[i]) {
        D3D11_SAMPLER_DESC sd;
        samplers[i]->GetDesc(&sd);
        auto rit = resolved_.find(f.base_address() & 0x1FFFFFFFu);
        BF2_TRACE("bf2trace:     src {} filter {:X} addr {} {} lod {} {} bias {} d3 {:08X} d4 {:08X} d5 {:08X}",
                    rit != resolved_.end() && rit->second.srv.Get() == srv ? "resolve" : "guest",
                    int(sd.Filter), int(sd.AddressU), int(sd.AddressV), sd.MinLOD, sd.MaxLOD,
                    sd.MipLODBias, f.d[3], f.d[4], f.d[5]);
      }
    }
    if (pixel) {
      ctx_->PSSetShaderResources(0, std::max(srv_count, 1u), srvs);
      if (n) ctx_->PSSetSamplers(0, n, samplers);
    } else {
      srvs[0] = mirror_srv_.Get();
      ctx_->VSSetShaderResources(0, std::max(srv_count, 1u), srvs);
      if (n) ctx_->VSSetSamplers(0, n, samplers);
    }
  };
  if (frame_ == trace_frame_) {
    BF2_TRACE("bf2trace: draw {} prim {} n {} vs {:08X} ps {:08X} keys {:016X} {:016X} rt {}x{} c0 {} fmt {} d {} vte {:X} vp {} {} {} {} mode {:X} dc {:X} blend0 {:08X} cmask {:X} cc {:X} pswr {:X} edram {}",
                frame_draws_, prim, host_indices.size(), LoadV(device + dev::kVertexShader), ps_object, vs->key,
                ps ? ps->key : 0,
                t.width, t.height, t.color[0] ? 1 : 0, t.color[0] ? t.color[0]->guest_format : 99,
                t.depth ? 1 : 0, Reg(device, 0x2206), vx, vy, vw, vh, mode, Reg(device, 0x2200),
                Reg(device, 0x2201), Reg(device, 0x2104), Reg(device, 0x2202),
                ps ? ps->info.color_written : 0, Reg(device, 0x2208));
    if (up) {
      const uint8_t* v = Virt(up->vertices);
      BF2_TRACE("bf2trace:   UP vtx {:08X} n {} stride {} idx {} | {} {} {} {} {} {} {} {}", up->vertices,
                  up->vertex_count, up->stride, up->indexed, AsFloat(Be32(v)), AsFloat(Be32(v + 4)),
                  AsFloat(Be32(v + 8)), AsFloat(Be32(v + 12)), AsFloat(Be32(v + 16)), AsFloat(Be32(v + 20)),
                  AsFloat(Be32(v + 24)), AsFloat(Be32(v + 28)));
      uint32_t bytes = up->vertex_count * up->stride;
      BF2_TRACE("bf2trace:   UP vf95 {:08X} {:08X} bytes {} hash {:016X}", st.xf[190], st.xf[191], bytes,
                  XXH3_64bits(v, bytes));
      if (!out_dir_.empty() && bytes) {
        for (int r = 0; r < 4; ++r) {
          const uint8_t* cr = d + dev::kVsConstants + r * 16;
          BF2_TRACE("bf2trace:   vsc[{}] {} {} {} {}", r, AsFloat(Be32(cr)), AsFloat(Be32(cr + 4)),
                      AsFloat(Be32(cr + 8)), AsFloat(Be32(cr + 12)));
        }
        std::ofstream f(out_dir_ / ("up_" + std::to_string(frame_draws_) + "_draw.bin"), std::ios::binary);
        f.write(reinterpret_cast<const char*>(v), bytes);
        up_dumps_.push_back({frame_draws_, up->vertices, bytes});
      }
    }
    BF2_TRACE("bf2trace:   cinfo {:08X} dinfo {:08X} surf {:08X} scale {} {} {} off {} {} {} zs {:X} xs {:X}",
                Reg(device, 0x2001), Reg(device, 0x2002), Reg(device, 0x2000), sx, sy, sz, ox, oy, oz,
                Reg(device, 0x2204), Reg(device, 0x2302));
  }
  // Render targets first: binding an SRV whose resource is still bound as an RTV (e.g. the
  // destination of the previous resolve) would be silently nulled by the runtime.
  PerfScope bindings_perf(11);
  BindTargets(t, 0xF);
  bind_textures(vs->info, false);
  if (ps) bind_textures(ps->info, true);
  bindings_perf.Stop();
  PerfScope pipeline_perf(12);

  // --- Output merger.
  uint32_t ps_writes = ps ? ps->info.color_written : 0;
  uint32_t color_mask = Reg(device, 0x2104);
  D3D11_BLEND_DESC bd;
  ZeroMemory(&bd, sizeof(bd));
  bd.AlphaToCoverageEnable = (colorcontrol >> 4) & 1;
  bd.IndependentBlendEnable = TRUE;
  bool const_color = false, const_alpha = false;
  static const uint32_t kBlendRegs[4] = {0x2201, 0x2209, 0x220A, 0x220B};
  uint32_t bound_mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    auto& rt = bd.RenderTarget[i];
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (!t.color[i]) continue;
    uint32_t mask = (color_mask >> (4 * i)) & 0xF;
    if (!(ps_writes & (1u << i))) mask = 0;
    rt.RenderTargetWriteMask = uint8_t(mask);
    if (mask) bound_mask |= 1u << i;
    uint32_t bc = Reg(device, kBlendRegs[i]);
    uint32_t cs = bc & 31, cop = (bc >> 5) & 7, cd = (bc >> 8) & 31;
    uint32_t as = (bc >> 16) & 31, aop = (bc >> 21) & 7, ad = (bc >> 24) & 31;
    bool off = cs == 1 && cd == 0 && cop == 0 && as == 1 && ad == 0 && aop == 0;
    uint32_t gf = t.color[i]->guest_format;
    if (off || gf == 14 || gf == 15) continue;
    rt.BlendEnable = TRUE;
    rt.SrcBlend = BlendFactor(cs, false);
    rt.DestBlend = BlendFactor(cd, false);
    rt.BlendOp = BlendOp(cop);
    rt.SrcBlendAlpha = BlendFactor(as, true);
    rt.DestBlendAlpha = BlendFactor(ad, true);
    rt.BlendOpAlpha = BlendOp(aop);
    for (uint32_t f : {cs, cd, as, ad}) {
      const_color |= f == 12 || f == 13;
      const_alpha |= f == 14 || f == 15;
    }
  }
  float blend_factor[4] = {RegF(device, 0x2105), RegF(device, 0x2106), RegF(device, 0x2107),
                           RegF(device, 0x2108)};
  if (const_alpha && !const_color) {
    blend_factor[0] = blend_factor[1] = blend_factor[2] = blend_factor[3];
  }
  BindTargets(t, bound_mask);
  auto* blend_state = GetBlend(bd);
  const bool blend_changed = pipeline_cache_.blend.Changed(blend_state);
  const bool factor_changed = pipeline_cache_.blend_factor.Changed(
      {blend_factor[0], blend_factor[1], blend_factor[2], blend_factor[3]});
  if (blend_changed || factor_changed)
    ctx_->OMSetBlendState(blend_state, blend_factor, 0xFFFFFFFF);

  uint32_t dc = Reg(device, 0x2200);
  uint32_t srm = Reg(device, 0x210D);
  D3D11_DEPTH_STENCIL_DESC dsd;
  ZeroMemory(&dsd, sizeof(dsd));
  if (t.depth) {
    dsd.DepthEnable = (dc >> 1) & 1;
    dsd.DepthWriteMask = ((dc >> 2) & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = D3D11_COMPARISON_FUNC(((dc >> 4) & 7) + 1);
    dsd.StencilEnable = dc & 1;
    dsd.StencilReadMask = uint8_t(srm >> 8);
    dsd.StencilWriteMask = uint8_t(srm >> 16);
    dsd.FrontFace.StencilFunc = D3D11_COMPARISON_FUNC(((dc >> 8) & 7) + 1);
    dsd.FrontFace.StencilFailOp = D3D11_STENCIL_OP(((dc >> 11) & 7) + 1);
    dsd.FrontFace.StencilPassOp = D3D11_STENCIL_OP(((dc >> 14) & 7) + 1);
    dsd.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP(((dc >> 17) & 7) + 1);
    if ((dc >> 7) & 1) {
      dsd.BackFace.StencilFunc = D3D11_COMPARISON_FUNC(((dc >> 20) & 7) + 1);
      dsd.BackFace.StencilFailOp = D3D11_STENCIL_OP(((dc >> 23) & 7) + 1);
      dsd.BackFace.StencilPassOp = D3D11_STENCIL_OP(((dc >> 26) & 7) + 1);
      dsd.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP(((dc >> 29) & 7) + 1);
    } else {
      dsd.BackFace = dsd.FrontFace;
    }
  } else {
    dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dsd.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                     D3D11_COMPARISON_ALWAYS};
    dsd.BackFace = dsd.FrontFace;
  }
  auto* depth_state = GetDepthStencil(dsd);
  const bool depth_changed = pipeline_cache_.depth.Changed(depth_state);
  const bool stencil_changed = pipeline_cache_.stencil.Changed(srm & 0xFF);
  if (depth_changed || stencil_changed) ctx_->OMSetDepthStencilState(depth_state, srm & 0xFF);

  // --- Rasterizer.
  D3D11_RASTERIZER_DESC rd;
  ZeroMemory(&rd, sizeof(rd));
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  if (kind == Kind::kTris) {
    if (mode & 1) rd.CullMode = D3D11_CULL_FRONT;
    else if (mode & 2) rd.CullMode = D3D11_CULL_BACK;
    if (((mode >> 3) & 3) && ((mode >> 5) & 7) == 1) rd.FillMode = D3D11_FILL_WIREFRAME;
  }
  rd.FrontCounterClockwise = !((mode >> 2) & 1);
  if (mode & (3u << 11)) {
    bool front = mode & (1u << 11);
    float scale = RegF(device, front ? 0x2380 : 0x2382);
    float offset = RegF(device, front ? 0x2381 : 0x2383);
    // The guest offset is a fraction of the depth range; D3D's constant bias counts units of the
    // depth format (2^-24 for 24-bit depth: D24S8 is unorm, D24FS8 float24 with a 2^-24 step
    // near 1). Without this the bias rounds to 0 and shadow maps acne all over the ground.
    bool float24 = (Reg(device, 0x2002) >> 16) & 1;
    float units = offset * (float24 ? 16777216.0f : 16777215.0f);
    rd.DepthBias = int(std::clamp(units, -2.0e9f, 2.0e9f));
    rd.SlopeScaledDepthBias = scale / 16.0f;
  }
  rd.DepthClipEnable = !((Reg(device, 0x2204) >> 16) & 1);
  rd.ScissorEnable = TRUE;
  auto* raster_state = GetRasterizer(rd);
  if (pipeline_cache_.raster.Changed(raster_state)) ctx_->RSSetState(raster_state);

  D3D11_VIEWPORT vp{vx * t.scale, vy * t.scale, vw * t.scale, vh * t.scale, 0.0f, 1.0f};
  if (pipeline_cache_.viewport.Changed(vp)) ctx_->RSSetViewports(1, &vp);
  // Scissor: screen scissor, the device's scissor rect when enabled, and the target.
  uint32_t tl = Reg(device, 0x200E), br = Reg(device, 0x200F);
  LONG x0 = LONG(tl & 0x7FFF), y0 = LONG((tl >> 16) & 0x7FFF);
  LONG x1 = LONG(br & 0x7FFF), y1 = LONG((br >> 16) & 0x7FFF);
  if (x1 <= x0 || y1 <= y0) {
    x0 = y0 = 0;
    x1 = y1 = 8192;
  }
  if (LoadV(device + 11952)) {
    LONG sx0 = LONG(LoadV(device + 12832)), sy0 = LONG(LoadV(device + 12836));
    LONG sx1 = LONG(LoadV(device + 12840)), sy1 = LONG(LoadV(device + 12844));
    if (sx1 > sx0 && sy1 > sy0 && sx1 <= 8192 && sy1 <= 8192) {
      x0 = std::max(x0, sx0);
      y0 = std::max(y0, sy0);
      x1 = std::min(x1, sx1);
      y1 = std::min(y1, sy1);
    }
  }
  D3D11_RECT sc{x0, y0, std::min<LONG>(x1, LONG(t.width)), std::min<LONG>(y1, LONG(t.height))};
  if (sc.right <= sc.left || sc.bottom <= sc.top) return;
  sc = ScaledRect(sc, t.scale);
  if (pipeline_cache_.scissor.Changed(sc)) ctx_->RSSetScissorRects(1, &sc);

  // --- Pipeline and draw.
  if (!pipeline_cache_.fixed_bindings) ctx_->IASetInputLayout(nullptr);
  const auto topology = native_strip ? (prim == 0x06 ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP
                                                                  : D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP)
                               : kind == Kind::kPoints  ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
                               : kind == Kind::kLines ? D3D11_PRIMITIVE_TOPOLOGY_LINELIST
                                                      : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
  if (pipeline_cache_.topology.Changed(topology)) ctx_->IASetPrimitiveTopology(topology);
  uint32_t count = uint32_t(host_indices.size());
  D3D11_MAP map = D3D11_MAP_WRITE_NO_OVERWRITE;
  if (index_pos_ + count > kIndexRingCount || index_pos_ == kIndexRingCount) {
    index_pos_ = 0;
    map = D3D11_MAP_WRITE_DISCARD;
  }
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(ctx_->Map(index_ring_.Get(), 0, map, 0, &m))) {
    index_pos_ = kIndexRingCount;  // retry with DISCARD after a failed upload
    return;
  }
  std::memcpy(static_cast<uint32_t*>(m.pData) + index_pos_, host_indices.data(), count * 4);
  ctx_->Unmap(index_ring_.Get(), 0);
  if (!pipeline_cache_.fixed_bindings) ctx_->IASetIndexBuffer(index_ring_.Get(), DXGI_FORMAT_R32_UINT, 0);

  ID3D11Buffer* vcbs[2] = {vs_consts_.Get(), state_cb_.Get()};
  ID3D11Buffer* pcbs[2] = {ps_consts_.Get(), state_cb_.Get()};
  if (pipeline_cache_.vs.Changed(vs->vs.Get())) ctx_->VSSetShader(vs->vs.Get(), nullptr, 0);
  if (!pipeline_cache_.fixed_bindings) {
    ctx_->VSSetConstantBuffers(0, 2, vcbs);
    ctx_->GSSetConstantBuffers(1, 1, pcbs + 1);
    ctx_->PSSetConstantBuffers(0, 2, pcbs);
    pipeline_cache_.fixed_bindings = true;
  }
  ID3D11GeometryShader* gs = kind == Kind::kPoints  ? gs_point_.Get()
                             : kind == Kind::kRects ? gs_rect_.Get()
                                                    : nullptr;
  if (pipeline_cache_.gs.Changed(gs)) ctx_->GSSetShader(gs, nullptr, 0);
  auto* pixel_shader = ps ? ps->ps.Get() : nullptr;
  if (pipeline_cache_.ps.Changed(pixel_shader)) ctx_->PSSetShader(pixel_shader, nullptr, 0);
  ComPtr<ID3D11Query> stats;
  if (frame_ == trace_frame_) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_PIPELINE_STATISTICS, 0};
    if (SUCCEEDED(dev_->CreateQuery(&qd, &stats))) ctx_->Begin(stats.Get());
  }
  ctx_->DrawIndexed(count, index_pos_, 0);
  index_pos_ += count;
  if (stats) {
    ctx_->End(stats.Get());
    draw_stats_.emplace_back(frame_draws_, stats);
  }

  HostRT* rt0 = t.color[0];
  if (step_on_ && frame_ == trace_frame_ && rt0 && rt0->width >= step_box_.right &&
      rt0->height >= step_box_.bottom) {
    D3D11_TEXTURE2D_DESC rd;
    rt0->tex->GetDesc(&rd);
    const uint64_t array_key = uint64_t(rd.Format) | (uint64_t(frame_draws_ / 1024) << 32);
    StepArray& a = step_arrays_[array_key];
    if (!a.tex) {
      D3D11_TEXTURE2D_DESC d = rd;
      d.Width = step_box_.right - step_box_.left;
      d.Height = step_box_.bottom - step_box_.top;
      d.MipLevels = 1;
      d.ArraySize = 1024;
      d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      d.MiscFlags = 0;
      if (SUCCEEDED(dev_->CreateTexture2D(&d, nullptr, &a.tex))) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd;
        rt0->srv->GetDesc(&sd);
        a.srv_format = sd.Format;
      }
    }
    if (a.tex && a.used < 1024) {
      ctx_->CopySubresourceRegion(a.tex.Get(), a.used, 0, 0, 0, rt0->tex.Get(), 0, &step_box_);
      steps_.push_back({frame_draws_, rt0, rd.Format, a.used++, array_key});
    }
  }
}

void Renderer::SaveSteps() {
  if (steps_.empty()) return;
  uint32_t w = step_box_.right - step_box_.left, h = step_box_.bottom - step_box_.top;
  std::map<const HostRT*, std::vector<uint8_t>> prev;
  uint32_t dumped = 0;
  for (const Step& st : steps_) {
    StepArray& a = step_arrays_[st.array_key];
    D3D11_TEXTURE2D_DESC d;
    a.tex->GetDesc(&d);
    d.ArraySize = 1;
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging, slice;
    if (FAILED(dev_->CreateTexture2D(&sd, nullptr, &staging)) || FAILED(dev_->CreateTexture2D(&d, nullptr, &slice)))
      break;
    imm_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, a.tex.Get(), st.slice, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(imm_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) break;
    // GPU row padding is undefined and may differ after every copy of identical pixels.
    // Comparing it used up the dump budget on unchanged draws and hid the actual culprit.
    const uint32_t bpp = (d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                          d.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ||
                          d.Format == DXGI_FORMAT_R32G32_FLOAT) ? 8 : 4;
    std::vector<uint8_t> cur(size_t(w) * bpp * h);
    for (uint32_t y = 0; y < h; ++y)
      std::memcpy(cur.data() + size_t(y) * w * bpp,
          static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(w) * bpp);
    imm_->Unmap(staging.Get(), 0);
    auto& p = prev[st.rt];
    size_t diff = 0;
    if (p.size() == cur.size())
      for (size_t k = 0; k < cur.size(); ++k) diff += cur[k] != p[k];
    else
      diff = cur.size();
    if (diff) {
      BF2_TRACE("bf2trace: step draw {} rt {} fmt {} changed {} bytes", st.draw, (const void*)st.rt,
                  int(st.format), diff);
      if (dumped < 300) {
        ComPtr<ID3D11ShaderResourceView> srv;
        D3D11_SHADER_RESOURCE_VIEW_DESC v{};
        v.Format = a.srv_format;
        v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        imm_->CopySubresourceRegion(slice.Get(), 0, 0, 0, 0, a.tex.Get(), st.slice, nullptr);
        if (SUCCEEDED(dev_->CreateShaderResourceView(slice.Get(), &v, &srv)))
          DumpSrv(srv.Get(), w, h, "step_" + std::to_string(st.draw));
        ++dumped;
      }
    }
    p.swap(cur);
  }
  steps_.clear();
  step_arrays_.clear();
}

// --- Clear -------------------------------------------------------------------------------------

void Renderer::ClearTargetsWithDraw(const Targets& t, const D3D11_RECT& guest_rect, uint32_t flags,
                                    const float color[4], float z, uint32_t stencil) {
  pipeline_cache_ = {};  // utility pass writes the deferred pipeline directly
  const D3D11_RECT rect = ScaledRect(guest_rect, t.scale);
  ID3D11RenderTargetView* rtvs[4] = {};
  for (uint32_t i = 0; i < 4; ++i) {
    if (t.color[i] && (flags & (1u << i))) rtvs[i] = t.color[i]->rtv.Get();
  }
  bool ds = t.depth && (flags & 0x30);
  ctx_->OMSetRenderTargets(4, rtvs, ds ? t.depth->dsv.Get() : nullptr);
  D3D11_BLEND_DESC bd;
  ZeroMemory(&bd, sizeof(bd));
  for (auto& rt : bd.RenderTarget) {
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  }
  ctx_->OMSetBlendState(GetBlend(bd), nullptr, 0xFFFFFFFF);
  D3D11_DEPTH_STENCIL_DESC dsd;
  ZeroMemory(&dsd, sizeof(dsd));
  dsd.DepthEnable = (flags & 0x10) != 0;
  dsd.DepthWriteMask = (flags & 0x10) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
  dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
  dsd.StencilEnable = (flags & 0x20) != 0;
  dsd.StencilReadMask = 0xFF;
  dsd.StencilWriteMask = 0xFF;
  dsd.FrontFace = {D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE,
                   D3D11_COMPARISON_ALWAYS};
  dsd.BackFace = dsd.FrontFace;
  ctx_->OMSetDepthStencilState(GetDepthStencil(dsd), stencil & 0xFF);
  D3D11_RASTERIZER_DESC rd;
  ZeroMemory(&rd, sizeof(rd));
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  rd.ScissorEnable = TRUE;
  ctx_->RSSetState(GetRasterizer(rd));
  D3D11_VIEWPORT vp{float(rect.left), float(rect.top), float(rect.right - rect.left),
                    float(rect.bottom - rect.top), 0.0f, 1.0f};
  ctx_->RSSetViewports(1, &vp);
  ctx_->RSSetScissorRects(1, &rect);
  UtilConsts u{};
  u.u0[0] = z;
  std::memcpy(u.u1, color, 16);
  SetUtil(u);
  ctx_->IASetInputLayout(nullptr);
  ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx_->VSSetShader(vs_fs_.Get(), nullptr, 0);
  ctx_->VSSetConstantBuffers(0, 1, util_cb_.GetAddressOf());
  ctx_->GSSetShader(nullptr, nullptr, 0);
  ctx_->PSSetShader(ps_clear_.Get(), nullptr, 0);
  ctx_->PSSetConstantBuffers(0, 1, util_cb_.GetAddressOf());
  ctx_->Draw(3, 0);
}

void Renderer::Clear(uint32_t device, uint32_t count, uint32_t rects, uint32_t flags,
                     uint32_t color, float z, uint32_t stencil) {
  Targets t;
  if (!CurrentTargets(device, t)) return;
  if (frame_ == trace_frame_) {
    BF2_TRACE("bf2trace: clear flags {:X} count {} color {:08X} z {} rt {}x{}", flags, count, color, z,
                t.width, t.height);
  }
  const float c[4] = {float((color >> 16) & 0xFF) / 255.0f, float((color >> 8) & 0xFF) / 255.0f,
                      float(color & 0xFF) / 255.0f, float(color >> 24) / 255.0f};
  if (!count || !rects) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (t.color[i] && (flags & (1u << i))) ctx_->ClearRenderTargetView(t.color[i]->rtv.Get(), c);
    }
    if (t.depth && (flags & 0x30)) {
      UINT f = ((flags & 0x10) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 0x20) ? D3D11_CLEAR_STENCIL : 0);
      ctx_->ClearDepthStencilView(t.depth->dsv.Get(), f, std::clamp(z, 0.0f, 1.0f),
                                  uint8_t(stencil));
    }
    return;
  }
  for (uint32_t i = 0; i < std::min(count, 64u); ++i) {
    uint32_t r = rects + i * 16;
    D3D11_RECT rect{LONG(int32_t(LoadV(r))), LONG(int32_t(LoadV(r + 4))),
                    LONG(int32_t(LoadV(r + 8))), LONG(int32_t(LoadV(r + 12)))};
    rect.left = std::max<LONG>(rect.left, 0);
    rect.top = std::max<LONG>(rect.top, 0);
    rect.right = std::min<LONG>(rect.right, LONG(t.width));
    rect.bottom = std::min<LONG>(rect.bottom, LONG(t.height));
    if (rect.right <= rect.left || rect.bottom <= rect.top) continue;
    ClearTargetsWithDraw(t, rect, flags, c, std::clamp(z, 0.0f, 1.0f), stencil);
  }
}

// --- Resolve -----------------------------------------------------------------------------------

void Renderer::Resolve(uint32_t device, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
                       uint32_t dest_point, uint32_t level, uint32_t slice, uint32_t clear_color,
                       float clear_z, uint32_t clear_stencil) {
  PerfScope perf(6);
  pipeline_cache_ = {};
  Targets t;
  if (!CurrentTargets(device, t)) return;
  uint32_t source = flags & 7;
  HostRT* src = source == 4 ? t.depth : (source < 4 ? t.color[source] : nullptr);
  if (frame_ == trace_frame_) {
    BF2_TRACE("bf2trace: resolve flags {:X} src {} rt {}x{} dest {:08X} -> {:08X} level {} slice {}",
                flags, src ? 1 : 0, t.width, t.height, dest_texture,
                dest_texture ? ReadFetch(dest_texture + 16).base_address() : 0, level, slice);
  }

  if (dest_texture && level == 0) {
    TexFetch f = ReadFetch(dest_texture + 16);
    if (f.base_address()) {
      if (Resolved* r = GetResolved(f, t.scale)) {
        LONG sx0 = 0, sy0 = 0, sx1 = LONG(t.width), sy1 = LONG(t.height);
        if (src_rect) {
          sx0 = LONG(int32_t(LoadV(src_rect)));
          sy0 = LONG(int32_t(LoadV(src_rect + 4)));
          sx1 = LONG(int32_t(LoadV(src_rect + 8)));
          sy1 = LONG(int32_t(LoadV(src_rect + 12)));
        }
        LONG dx = sx0, dy = sy0;
        if (dest_point) {
          dx = LONG(int32_t(LoadV(dest_point)));
          dy = LONG(int32_t(LoadV(dest_point + 4)));
        } else if (!src_rect) {
          dx = dy = 0;
        }
        LONG w = std::min(sx1 - sx0, LONG(r->width) - dx);
        LONG h = std::min(sy1 - sy0, LONG(r->height) - dy);
        uint32_t s = std::min(slice, r->slices - 1);
        if (src && w > 0 && h > 0 && r->rtvs[s]) {
          ID3D11RenderTargetView* rtv = r->rtvs[s].Get();
          ctx_->OMSetRenderTargets(1, &rtv, nullptr);
          D3D11_BLEND_DESC bd;
  ZeroMemory(&bd, sizeof(bd));
          bd.RenderTarget[0].SrcBlend = bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
          bd.RenderTarget[0].DestBlend = bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
          bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
          bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
          ctx_->OMSetBlendState(GetBlend(bd), nullptr, 0xFFFFFFFF);
          D3D11_DEPTH_STENCIL_DESC dsd;
  ZeroMemory(&dsd, sizeof(dsd));
          dsd.DepthFunc = D3D11_COMPARISON_ALWAYS;
          dsd.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP,
                           D3D11_COMPARISON_ALWAYS};
          dsd.BackFace = dsd.FrontFace;
          ctx_->OMSetDepthStencilState(GetDepthStencil(dsd), 0);
          D3D11_RASTERIZER_DESC rd;
  ZeroMemory(&rd, sizeof(rd));
          rd.FillMode = D3D11_FILL_SOLID;
          rd.CullMode = D3D11_CULL_NONE;
          rd.DepthClipEnable = TRUE;
          rd.ScissorEnable = TRUE;
          ctx_->RSSetState(GetRasterizer(rd));
          D3D11_RECT sc = ScaledRect({dx, dy, dx + w, dy + h}, t.scale);
          D3D11_VIEWPORT vp{float(sc.left), float(sc.top), float(sc.right - sc.left), float(sc.bottom - sc.top),
                            0.0f, 1.0f};
          ctx_->RSSetViewports(1, &vp);
          ctx_->RSSetScissorRects(1, &sc);
          // The resolve stores 8_8_8_8 in memory as BGRA: games sample it with a swizzle that
          // swaps red and blue back (fetch swizzle 60A), so host textures keep the same order.
          bool swap = source < 4 && src->guest_format <= 1 && (f.format() == 6 || f.format() == 62);
          UtilConsts u{};
          u.u2[0] = float(ScaledCoord(sx0, t.scale) - ScaledCoord(dx, t.scale));
          u.u2[1] = float(ScaledCoord(sy0, t.scale) - ScaledCoord(dy, t.scale));
          u.u2[2] = swap ? 1.0f : 0.0f;
          SetUtil(u);
          r->bgra = swap;
          ctx_->IASetInputLayout(nullptr);
          ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          ctx_->VSSetShader(vs_fs_.Get(), nullptr, 0);
          ctx_->VSSetConstantBuffers(0, 1, util_cb_.GetAddressOf());
          ctx_->GSSetShader(nullptr, nullptr, 0);
          ctx_->PSSetShader(ps_copy_.Get(), nullptr, 0);
          ctx_->PSSetConstantBuffers(0, 1, util_cb_.GetAddressOf());
          ID3D11ShaderResourceView* srv = src->srv.Get();
          ctx_->PSSetShaderResources(0, 1, &srv);
          ctx_->Draw(3, 0);
          ID3D11ShaderResourceView* none = nullptr;
          ctx_->PSSetShaderResources(0, 1, &none);
          // Unbind the destination: D3D11 nulls an SRV bound while its resource is still an RTV.
          ctx_->OMSetRenderTargets(0, nullptr, nullptr);
          r->used_width = uint32_t(dx + w);
          r->used_height = uint32_t(dy + h);
        }
        r->signature = GuestSignature(f);
        r->checked_frame = frame_;
      }
    }
  }

  // Optional clears of the source surfaces after the copy.
  if ((flags & 0x100) && clear_color) {
    float c[4];
    for (int i = 0; i < 4; ++i) c[i] = AsFloat(LoadV(clear_color + i * 4));
    HostRT* rt = source < 4 ? t.color[source] : t.color[0];
    if (rt) ctx_->ClearRenderTargetView(rt->rtv.Get(), c);
  }
  if ((flags & 0x200) && t.depth) {
    ctx_->ClearDepthStencilView(t.depth->dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL,
                                std::clamp(clear_z, 0.0f, 1.0f), uint8_t(clear_stencil));
  }
}

// Re-uploads textures sampled this frame whose guest memory the CPU changed since they were
// decoded (video frames, dynamic textures). Runs on the immediate context before the frame's
// command list, so the recorded draws see the final contents.
void Renderer::RefreshTextures() {
  PerfScope perf(2);
  const uint8_t* phys = Mem()->physical_membase();
  texture_hash_inputs_.clear();
  for (uint64_t k : frame_textures_) {
    auto it = textures_.find(k);
    if (it == textures_.end() || !it->second.res) continue;
    texture_hash_inputs_.push_back(&it->second);
  }
  texture_hash_results_.resize(texture_hash_inputs_.size());
  {
    PerfScope hash_perf(7);
    texture_hash_jobs_.Run(texture_hash_inputs_.size(), [&](size_t i) {
      texture_hash_results_[i] = HashTextureMemory(phys, texture_hash_inputs_[i]->fetch);
    });
  }
  for (size_t i=0;i<texture_hash_inputs_.size();++i) {
    HostTex& t = *texture_hash_inputs_[i];
    uint64_t h = texture_hash_results_[i];
    if (h == t.mem_hash) continue;
    HostTexture ht;
    PerfScope decode_perf(8);
    if (!DecodeTexture(phys, t.fetch, ht)) continue;
    decode_perf.Stop();
    PerfScope upload_perf(9);
    uint32_t array = ht.is_3d ? 1 : ht.depth;
    for (uint32_t s = 0; s < array; ++s) {
      for (uint32_t l = 0; l < t.levels && l < ht.levels; ++l) {
        const auto& sub = ht.subs[size_t(s) * ht.levels + l];
        if (sub.data.empty()) continue;
        imm_->UpdateSubresource(t.res.Get(), D3D11CalcSubresource(l, s, t.levels), nullptr,
                                sub.data.data(), sub.row_pitch, sub.slice_pitch);
      }
    }
    t.mem_hash = h;  // do not cache a failed decode as if it had reached the GPU
  }
  frame_textures_.clear();
}

// Debug aid: 24-bit BMP from RGBA8 rows, written next to the executable.
void Renderer::WriteBmp(const std::string& name, const uint8_t* rgba, uint32_t pitch,
                        uint32_t w, uint32_t h) {
  uint32_t row = w * 3, padded = (row + 3) & ~3u;
  std::vector<uint8_t> px(size_t(padded) * h);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* src = rgba + size_t(pitch) * (h - 1 - y);
    for (uint32_t x = 0; x < w; ++x) {
      px[size_t(y) * padded + x * 3 + 0] = src[x * 4 + 2];
      px[size_t(y) * padded + x * 3 + 1] = src[x * 4 + 1];
      px[size_t(y) * padded + x * 3 + 2] = src[x * 4 + 0];
    }
  }
  BITMAPFILEHEADER fh{};
  BITMAPINFOHEADER ih{};
  ih.biSize = sizeof(ih);
  ih.biWidth = LONG(w);
  ih.biHeight = LONG(h);
  ih.biPlanes = 1;
  ih.biBitCount = 24;
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof(fh) + sizeof(ih);
  fh.bfSize = DWORD(fh.bfOffBits + px.size());
  std::ofstream out(out_dir_ / name, std::ios::binary);
  out.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
  out.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
  out.write(reinterpret_cast<const char*>(px.data()), std::streamsize(px.size()));
}

// Debug aid: writes the back buffer to frame_<n>.bmp next to the executable.
void Renderer::DumpBackBuffer() {
  ComPtr<ID3D11Texture2D> back;
  swap_->GetBuffer(0, IID_PPV_ARGS(&back));
  D3D11_TEXTURE2D_DESC d;
  back->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  d.MiscFlags = 0;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(dev_->CreateTexture2D(&d, nullptr, &staging))) return;
  imm_->CopyResource(staging.Get(), back.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(imm_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
  char name[48];
  snprintf(name, sizeof(name), "frame_%06u.bmp", frame_);
  WriteBmp(name, static_cast<const uint8_t*>(m.pData), m.RowPitch, d.Width, d.Height);
  imm_->Unmap(staging.Get(), 0);
}

// Debug aid: logs the center texel (slice 0) of a target as floats.
void Renderer::LogCenterTexel(ID3D11Texture2D* tex, const char* name) {
  D3D11_TEXTURE2D_DESC d;
  tex->GetDesc(&d);
  D3D11_BOX box{d.Width / 2, d.Height / 2, 0, d.Width / 2 + 1, d.Height / 2 + 1, 1};
  D3D11_TEXTURE2D_DESC sd{};
  sd.Width = sd.Height = 1;
  sd.MipLevels = sd.ArraySize = 1;
  sd.Format = d.Format;
  sd.SampleDesc.Count = 1;
  sd.Usage = D3D11_USAGE_STAGING;
  sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> st;
  if (FAILED(dev_->CreateTexture2D(&sd, nullptr, &st))) return;
  imm_->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, tex, 0, &box);
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(imm_->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
  const uint8_t* p = static_cast<const uint8_t*>(m.pData);
  float f[4] = {};
  auto half = [](uint16_t h) {
    uint32_t e = (h >> 10) & 31, mt = h & 1023;
    float v = e ? std::ldexp(1.0f + mt / 1024.0f, int(e) - 15) : std::ldexp(mt / 1024.0f, -14);
    if (e == 31) v = mt ? NAN : INFINITY;
    return (h & 0x8000) ? -v : v;
  };
  switch (d.Format) {
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
      std::memcpy(f, p, 4);
      break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
      for (int i = 0; i < 4; ++i) f[i] = half(uint16_t(p[i * 2] | (p[i * 2 + 1] << 8)));
      break;
    default:
      for (int i = 0; i < 4; ++i) f[i] = p[i] / 255.0f;
      break;
  }
  BF2_TRACE("bf2dump: {} center {} {} {} {}", name, f[0], f[1], f[2], f[3]);
  imm_->Unmap(st.Get(), 0);
}

// Debug aid: draws a 2D view into an RGBA8 target and writes it to <name>.bmp.
void Renderer::DumpSrv(ID3D11ShaderResourceView* srv, uint32_t w, uint32_t h,
                       const std::string& name) {
  if (!srv || !w || !h) return;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = w;
  d.Height = h;
  d.MipLevels = 1;
  d.ArraySize = 1;
  d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1;
  d.Usage = D3D11_USAGE_DEFAULT;
  d.BindFlags = D3D11_BIND_RENDER_TARGET;
  ComPtr<ID3D11Texture2D> tex, staging;
  ComPtr<ID3D11RenderTargetView> rtv;
  if (FAILED(dev_->CreateTexture2D(&d, nullptr, &tex))) return;
  dev_->CreateRenderTargetView(tex.Get(), nullptr, &rtv);
  imm_->ClearState();
  ID3D11RenderTargetView* r = rtv.Get();
  imm_->OMSetRenderTargets(1, &r, nullptr);
  D3D11_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
  D3D11_RECT sc{0, 0, LONG(w), LONG(h)};
  imm_->RSSetViewports(1, &vp);
  imm_->RSSetScissorRects(1, &sc);
  UtilConsts u{};
  u.u3[0] = u.u3[1] = 1.0f;
  UpdateBuffer(imm_.Get(), util_cb_.Get(), &u, sizeof(u));
  imm_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  imm_->VSSetShader(vs_fs_.Get(), nullptr, 0);
  imm_->VSSetConstantBuffers(0, 1, util_cb_.GetAddressOf());
  imm_->PSSetShader(ps_present_.Get(), nullptr, 0);
  imm_->PSSetConstantBuffers(0, 1, util_cb_.GetAddressOf());
  imm_->PSSetShaderResources(0, 1, &srv);
  imm_->PSSetSamplers(0, 1, linear_clamp_.GetAddressOf());
  imm_->Draw(3, 0);
  imm_->ClearState();
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (FAILED(dev_->CreateTexture2D(&d, nullptr, &staging))) return;
  imm_->CopyResource(staging.Get(), tex.Get());
  D3D11_MAPPED_SUBRESOURCE m;
  if (FAILED(imm_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
  WriteBmp(name + ".bmp", static_cast<const uint8_t*>(m.pData), m.RowPitch, w, h);
  imm_->Unmap(staging.Get(), 0);
}

// --- Swap --------------------------------------------------------------------------------------

// F12: the next frame is the capture (see capture_request).
void Renderer::BeginCapture() {
  // Optional live diagnostic probe; avoids restarting the game to move the sampled pixel.
  {
    std::ifstream probe(cache_dir_.parent_path() / "step-region.txt");
    unsigned x,y,w,h;
    if (probe >> x >> y >> w >> h; probe && w && h && w <= 64 && h <= 64) {
      step_box_ = D3D11_BOX{x,y,0,x+w,y+h,1};
      step_on_ = true;
    }
  }
  SYSTEMTIME t;
  GetLocalTime(&t);
  char name[64];
  snprintf(name, sizeof(name), "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour,
           t.wMinute, t.wSecond);
  std::filesystem::path dir = cache_dir_.parent_path() / "captures" / name;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) return;
  out_dir_ = dir;
  trace_out_.open(dir / "trace.txt");
  capture_frame_ = trace_frame_ = dump_targets_frame_ = frame_;
  REXLOG_INFO("bf2native: F12 capture of frame {} -> {}", frame_, dir.string());
}

// Capture: every texture sampled this frame, levels 0..3 (2D, single slice), as
// tex_<base>_f<format>_<w>x<h>_m<level>.bmp, and its fetch constant in trace.txt.
void Renderer::DumpFrameTextures() {
  std::vector<uint64_t> keys = frame_textures_;
  std::sort(keys.begin(), keys.end());
  keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
  for (uint64_t k : keys) {
    auto it = textures_.find(k);
    if (it == textures_.end() || !it->second.res) continue;
    HostTex& t = it->second;
    const TexFetch& f = t.fetch;
    BF2_TRACE("bf2trace: texture {:08X} fmt {} {}x{}x{} dim {} tiled {} levels {} mip {:08X} packed {} "
              "minmip {} maxmip {} filter mag {} min {} mip {} aniso {} bias {} | {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
              f.base_address(), f.format(), f.width(), f.height(), f.depth(), f.dimension(), f.tiled(),
              t.levels, f.mip_address(), f.packed_mips(), f.mip_min_level(), f.mip_max_level(),
              f.mag_filter(), f.min_filter(), f.mip_filter(), f.aniso_filter(), f.lod_bias(), f.d[0],
              f.d[1], f.d[2], f.d[3], f.d[4], f.d[5]);
    if (f.dimension() == 3 && f.format() == 6) {
      HostTexture cube;
      if (DecodeTexture(Mem()->physical_membase(), f, cube) && cube.format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        for (uint32_t face = 0; face < 6; ++face) {
          const auto& sub = cube.subs[face*cube.levels];
          char name[80];
          snprintf(name,sizeof(name),"cube_%08X_face%u",f.base_address(),face);
          if (!sub.data.empty()) WriteBmp(name,sub.data.data(),sub.row_pitch,cube.width,cube.height);
        }
      }
    }
    ComPtr<ID3D11Texture2D> tex2d;
    if (FAILED(t.res.As(&tex2d))) continue;
    D3D11_TEXTURE2D_DESC td;
    tex2d->GetDesc(&td);
    if (td.ArraySize != 1 || (td.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)) continue;
    for (uint32_t l = 0; l < t.levels && l < 4; ++l) {
      D3D11_SHADER_RESOURCE_VIEW_DESC v{};
      v.Format = td.Format;
      v.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      v.Texture2D.MostDetailedMip = l;
      v.Texture2D.MipLevels = 1;
      ComPtr<ID3D11ShaderResourceView> srv;
      if (FAILED(dev_->CreateShaderResourceView(tex2d.Get(), &v, &srv))) break;
      uint32_t w = std::max(1u, td.Width >> l), h = std::max(1u, td.Height >> l);
      char name[80];
      snprintf(name, sizeof(name), "tex_%08X_f%u_%ux%u_m%u", f.base_address(), f.format(), td.Width,
               td.Height, l);
      DumpSrv(srv.Get(), w, h, name);
    }
  }
}

void Renderer::Swap(uint32_t device, uint32_t front_texture) {
  (void)device;
  bool capturing = frame_ == capture_frame_;
  if (capturing) DumpFrameTextures();  // before RefreshTextures forgets this frame's list
  RefreshTextures();
  // Traced frame: UP vertex data as it is when the frame executes (compare with up_<n>_draw.bin).
  for (const UpDump& u : up_dumps_) {
    const uint8_t* v = Virt(u.vertices);
    uint64_t h = XXH3_64bits(v, u.bytes);
    BF2_TRACE("bf2trace: UP draw {} at swap hash {:016X}", u.draw, h);
    std::ofstream f(out_dir_ / ("up_" + std::to_string(u.draw) + "_swap.bin"), std::ios::binary);
    f.write(reinterpret_cast<const char*>(v), u.bytes);
  }
  up_dumps_.clear();
  LateSyncMirror();
  {
    ComPtr<ID3D11CommandList> list;
    if (SUCCEEDED(ctx_->FinishCommandList(FALSE, &list))) imm_->ExecuteCommandList(list.Get(), FALSE);
    // Each command list must start with a WRITE_DISCARD map for every dynamic buffer.
    for (auto& snapshot : constant_snapshots_) snapshot.valid = false;
    pipeline_cache_ = {};
    index_pos_ = kIndexRingCount;  // the next command list must start with a discarding map
  }
  for (auto& [draw, q] : draw_stats_) {
    D3D11_QUERY_DATA_PIPELINE_STATISTICS ps{};
    while (imm_->GetData(q.Get(), &ps, sizeof(ps), 0) == S_FALSE) std::this_thread::yield();
    BF2_TRACE("bf2stats: draw {} ia_prims {} clip_prims {} ps {}", draw, ps.IAPrimitives, ps.CPrimitives,
              ps.PSInvocations);
  }
  draw_stats_.clear();
  if (frame_ == dump_targets_frame_) {
    SaveSteps();
    char name[64];
    for (auto& [base, res] : resolved_) {
      snprintf(name, sizeof(name), "res_%08X_f%u_%ux%u", base, res.guest_format, res.width, res.height);
      DumpSrv(res.srv_slice0.Get(), res.width, res.height, name);
      LogCenterTexel(res.tex.Get(), name);
    }
    for (auto& [key, rt] : rts_) {
      snprintf(name, sizeof(name), "rt_%s_%03X_f%u_%ux%u", rt.depth ? "d" : "c", uint32_t(key & 0xFFF),
               rt.guest_format, rt.width, rt.height);
      DumpSrv(rt.srv.Get(), rt.width, rt.height, name);
      LogCenterTexel(rt.tex.Get(), name);
    }
  }
  Resolved* r = nullptr;
  if (front_texture) {
    TexFetch f = ReadFetch(front_texture + 16);
    last_front_base_ = f.base_address() & 0x1FFFFFFFu;
  }
  auto it = resolved_.find(last_front_base_);
  if (it != resolved_.end()) r = &it->second;
  // Final picture, settings menu, vsync and frame pacing: pc_settings.cpp.
  pcs::Picture picture;
  if (r && r->srv_slice0) {
    uint32_t sw = r->used_width ? r->used_width : r->width;
    uint32_t sh = r->used_height ? r->used_height : r->height;
    picture.srv = r->srv_slice0.Get();
    picture.width = float(sw);
    picture.height = float(sh);
    picture.u1 = float(sw) / float(r->width);
    picture.v1 = float(sh) / float(r->height);
    picture.swap_rb = r->bgra;  // front buffers come from resolves (BGRA for 8_8_8_8)
    if (capturing) DumpSrv(r->srv_slice0.Get(), sw, sh, "frame");
    if (dump_every_ && frame_ % dump_every_ == 0) {
      char name[64];
      snprintf(name, sizeof(name), "frame_%06llu", static_cast<unsigned long long>(frame_));
      DumpSrv(r->srv_slice0.Get(), sw, sh, name);
    }
  }
  imm_->ClearState();
  // A new internal resolution from the settings menu takes effect from the next frame: targets
  // and resolve destinations of another scale are recreated as they are used.
  if (float s = pcs::ResolutionScale(); s != scale_) {
    REXLOG_INFO("bf2native: internal resolution x{:.3f} -> x{:.3f}", scale_, s);
    scale_ = s;
  }
  pcs::Present(dev_.Get(), imm_.Get(), swap_.Get(), picture,
               [this](ID3D11RenderTargetView** rtv, uint32_t* w, uint32_t* h) {
                 RECT cr{};
                 GetClientRect(hwnd_, &cr);
                 uint32_t cw = uint32_t(std::max<LONG>(cr.right - cr.left, 1));
                 uint32_t ch = uint32_t(std::max<LONG>(cr.bottom - cr.top, 1));
                 if (cw != back_w_ || ch != back_h_ || !back_rtv_) {
                   back_rtv_.Reset();
                   imm_->OMSetRenderTargets(0, nullptr, nullptr);
                   imm_->ClearState();
                   imm_->Flush();
                   if (FAILED(swap_->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, pcs::SwapChainFlags())))
                     return false;
                   ComPtr<ID3D11Texture2D> back;
                   swap_->GetBuffer(0, IID_PPV_ARGS(&back));
                   dev_->CreateRenderTargetView(back.Get(), nullptr, &back_rtv_);
                   back_w_ = cw;
                   back_h_ = ch;
                 }
                 *rtv = back_rtv_.Get();
                 *w = back_w_;
                 *h = back_h_;
                 return back_rtv_ != nullptr;
               });

  if (frame_ <= 5 || frame_ % 600 == 0) {
    REXLOG_INFO("bf2native: frame {}: {} draws ({} skipped), front {:08X}{}", frame_, frame_draws_,
                frame_skipped_, last_front_base_, r ? "" : " (not resolved)");
  }
  frame_draws_ = frame_skipped_ = 0;
  if (capturing) {
    capture_frame_ = UINT32_MAX;
    trace_out_.close();
    out_dir_ = cache_dir_.parent_path();
    REXLOG_INFO("bf2native: F12 capture done");
  }
  ++frame_;
  PerfScope::Report(frame_);
  if (capture_request.exchange(false)) BeginCapture();
  if (debug_at_sec_ && std::chrono::steady_clock::now() - init_time_ >= std::chrono::seconds(debug_at_sec_)) {
    debug_at_sec_ = 0;
    trace_frame_ = dump_targets_frame_ = frame_;
    REXLOG_INFO("bf2native: debug capture of frame {}", frame_);
  }

}

}  // namespace

// --- Entry points ------------------------------------------------------------------------------

bool RendererInit(void* hwnd) {
  if (g_renderer) return true;
  if (!hwnd || !IsWindow(static_cast<HWND>(hwnd))) {
    REXLOG_ERROR("bf2native: no valid window handle for the renderer");
    return false;
  }
  timeBeginPeriod(1);
  auto r = std::make_unique<Renderer>();
  if (!r->Init(static_cast<HWND>(hwnd))) return false;
  g_renderer = r.release();
  return true;
}

void RendererDraw(uint32_t device, uint32_t prim, int32_t base_vertex, uint32_t start,
                  uint32_t count, bool indexed) {
  if (!g_renderer || !count) return;
  std::lock_guard<std::mutex> lock(g_renderer->mutex);
  static thread_local std::vector<uint32_t> vids;
  vids.resize(count);
  if (indexed) {
    uint32_t ib = LoadV(device + dev::kIndexBuffer);
    if (!ib) return;
    bool is32 = LoadV(ib) >> 31;
    uint32_t address = LoadV(ib + 0xC);
    if (!address) return;
    uint32_t mode = Reg(device, 0x2205);
    bool restart = (mode >> 21) & 1;
    uint32_t reset = Reg(device, 0x2103) & (is32 ? 0xFFFFFFu : 0xFFFFu);
    const uint8_t* p = Virt(address + start * (is32 ? 4 : 2));
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t v = is32 ? Be32(p + i * 4) : Be16(p + i * 2);
      vids[i] = (restart && (v & (is32 ? 0xFFFFFFu : 0xFFFFu)) == reset)
                    ? UINT32_MAX
                    : uint32_t((int32_t(v) + base_vertex) & 0xFFFFFF);
    }
  } else {
    for (uint32_t i = 0; i < count; ++i) vids[i] = uint32_t(base_vertex) + i;
  }
  g_renderer->Draw(device, prim, vids, nullptr);
}

void RendererDrawUp(uint32_t device, const UpDraw& up) {
  if (!g_renderer || !up.vertices || !up.stride) return;
  std::lock_guard<std::mutex> lock(g_renderer->mutex);
  static thread_local std::vector<uint32_t> vids;
  if (up.indexed) {
    if (!up.indices || !up.index_count) return;
    vids.resize(up.index_count);
    const uint8_t* p = Virt(up.indices);
    for (uint32_t i = 0; i < up.index_count; ++i) {
      vids[i] = up.index32 ? Be32(p + i * 4) : Be16(p + i * 2);
    }
  } else {
    if (!up.vertex_count) return;
    vids.resize(up.vertex_count);
    for (uint32_t i = 0; i < up.vertex_count; ++i) vids[i] = i;
  }
  g_renderer->Draw(device, up.prim, vids, &up);
}

void RendererClear(uint32_t device, uint32_t count, uint32_t rects, uint32_t flags,
                   uint32_t color, float z, uint32_t stencil) {
  if (!g_renderer) return;
  std::lock_guard<std::mutex> lock(g_renderer->mutex);
  g_renderer->Clear(device, count, rects, flags, color, z, stencil);
}

void RendererResolve(uint32_t device, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
                     uint32_t dest_point, uint32_t level, uint32_t slice, uint32_t clear_color,
                     float clear_z, uint32_t clear_stencil) {
  if (!g_renderer) return;
  std::lock_guard<std::mutex> lock(g_renderer->mutex);
  g_renderer->Resolve(device, flags, src_rect, dest_texture, dest_point, level, slice, clear_color,
                      clear_z, clear_stencil);
}

void RendererRequestCapture() {
  if (g_renderer) g_renderer->capture_request = true;
}

void RendererSwap(uint32_t device, uint32_t front_texture) {
  if (!g_renderer) return;
  std::lock_guard<std::mutex> lock(g_renderer->mutex);
  g_renderer->Swap(device, front_texture);
}

}  // namespace bf2n
