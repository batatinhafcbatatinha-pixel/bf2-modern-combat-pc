// bf2mc native GPU - interface between the guest D3D hooks (gpu_hooks.cpp) and the host D3D11
// renderer (renderer.cpp).
//
// There is no GPU emulation: the game's statically linked Direct3D library still builds its
// command buffers on the CPU (they are never executed), and every operation that produces GPU work
// (draw, clear, resolve, swap) is replaced by a host Direct3D 11 implementation that reads the
// D3D device's register shadow (see analysis/d3d-map.md).

#pragma once

#include <cstdint>
#include <vector>

namespace bf2n {

// Guest device offsets (analysis/d3d-map.md).
namespace dev {
constexpr uint32_t kFetchConstants = 0x480;   // 32 x 6 dwords, regs 0x4800..
constexpr uint32_t kVsConstants = 0x780;      // 256 float4, regs 0x4000..
constexpr uint32_t kPsConstants = 0x1780;     // 256 float4, regs 0x4400..
constexpr uint32_t kBoolLoopConstants = 0x2780;  // 8 bool + 32 loop dwords, regs 0x4900..
constexpr uint32_t kFlags10433 = 0x28C1;      // byte: bit 2 = GPU-less (CPU-written fences)
constexpr uint32_t kColorSurfaces = 0x30F8;   // 12536: 4 surface pointers
constexpr uint32_t kDepthSurface = 0x3108;    // 12552
constexpr uint32_t kIndexBuffer = 0x30F4;     // 12532
constexpr uint32_t kPixelShader = 0x3290;
constexpr uint32_t kVertexShader = 0x3294;
constexpr uint32_t kVertexDeclaration = 0x2C90;  // 11408
constexpr uint32_t kGpuCallbackBlock = 0x2894;  // 10388: pointer to {mask, ?, ?, ?, callback, data}
}  // namespace dev

// The draw flush (sub_8248AF68) patches the vertex shader's vfetch instructions in memory for the
// current vertex declaration only when the GPU is past the shader's last use; otherwise
// sub_8248AB00 copies the microcode into the command buffer, patches the copy and leaves memory
// patched for the previous declaration. The copy is the loaded shader while that declaration is
// current (gpu_hooks.cpp keeps it; Renderer::ReadUcode uses it).
struct ImmediateVertexShader {
  uint32_t device = 0, shader = 0, declaration = 0, declaration_id = 0;
  std::vector<uint8_t> code;
};
inline ImmediateVertexShader& LoadedImmediateVertexShader() {
  static ImmediateVertexShader s;
  return s;
}

struct UpDraw {
  bool pending = false;
  bool indexed = false;
  uint32_t prim = 0;
  uint32_t vertex_count = 0;
  uint32_t index_count = 0;
  uint32_t stride = 0;        // bytes
  uint32_t vertices = 0;      // guest virtual address
  uint32_t indices = 0;       // guest virtual address
  bool index32 = false;
  int32_t base_vertex = 0;
};

// Renderer entry points, called on the guest render thread.
bool RendererInit(void* hwnd);
void RendererDraw(uint32_t device, uint32_t prim, int32_t base_vertex, uint32_t start,
                  uint32_t count, bool indexed);
void RendererDrawUp(uint32_t device, const UpDraw& up);
void RendererClear(uint32_t device, uint32_t count, uint32_t rects, uint32_t flags,
                   uint32_t color, float z, uint32_t stencil);
void RendererResolve(uint32_t device, uint32_t flags, uint32_t src_rect, uint32_t dest_texture,
                     uint32_t dest_point, uint32_t level, uint32_t slice, uint32_t clear_color,
                     float clear_z, uint32_t clear_stencil);
void RendererSwap(uint32_t device, uint32_t front_texture);
// F12: trace and dump the next frame into captures/<time>/ next to the executable (any thread).
void RendererRequestCapture();

// gpu_hooks.cpp
void NativeGpuInstallMmio();
void NativeGpuSetWindow(void* hwnd);

}  // namespace bf2n
