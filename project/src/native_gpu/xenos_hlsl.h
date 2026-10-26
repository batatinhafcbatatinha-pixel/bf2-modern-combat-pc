// bf2mc native GPU - Xenos shader microcode -> HLSL (Shader Model 5) translator.
//
// The game's shaders are Xenos microcode (control flow + ALU + fetch instructions, see the
// R400/Xenos ISA notes in rex/graphics/format/ucode.h). This translates them to HLSL source that
// the native D3D11 renderer compiles; nothing interprets microcode at run time.
//
// Binding model of the generated code:
//   b0  cbuffer XConsts  float4 c[256]          VS: regs 0x4000.., PS: 0x4400..
//   b1  cbuffer XState   bools, loops, fetch constants, renderer system values
//   t0  ByteAddressBuffer xvmem                 VS: vertex data (fetch constant addresses rebased)
//   t1+i / s0+i                                 texture binding i (ShaderInfo::textures[i])

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bf2n {

enum class TexDim : uint8_t { k1D = 0, k2D = 1, k3D = 2, kCube = 3 };

struct TextureBinding {
  uint32_t slot = 0;  // texture fetch constant 0..31
  TexDim dim = TexDim::k2D;
  // Filter overrides from the fetch instruction; 0xFF = take from the fetch constant.
  uint8_t mag = 0xFF, min = 0xFF, mip = 0xFF, aniso = 0xFF;
};

struct ShaderInfo {
  bool pixel = false;
  bool ok = false;
  std::string error;
  std::vector<TextureBinding> textures;
  uint64_t vfetch_used[2] = {};  // vertex fetch constants 0..95 read by the shader
  uint32_t color_written = 0;    // pixel: oC0..3 mask
  bool writes_depth = false;
  bool uses_kill = false;
  bool writes_point_size = false;
  bool uses_memexport = false;
  uint32_t interp_written = 0;  // vertex: o0..o15 mask
  uint32_t reg_count = 0;
};

struct TranslateOptions {
  // Pixel shader: GPR that receives PsParamGen (SQ_CONTEXT_MISC.param_gen_pos), or ~0u.
  uint32_t ps_param_gen_reg = ~0u;
  // Preserve Xenos 20e4 precision before host depth testing/writes.
  bool depth20e4 = false;
};

// `ucode` is in host byte order. Returns HLSL with entry point "main".
std::string TranslateXenosShader(const uint32_t* ucode, uint32_t dword_count, bool pixel,
                                 const TranslateOptions& options, ShaderInfo& info);

// Number of 32-bit system values (xsys) shared by all generated shaders, see XState in the
// generated header.
constexpr uint32_t kSysFloat4Count = 8;

// 3D-or-stacked fetches (TexDim::k3D) also get a Texture2DArray at t(1 + i + kStackedSrvBase),
// used when the bound fetch constant is a 2D / stacked texture.
constexpr uint32_t kStackedSrvBase = 16;

}  // namespace bf2n
