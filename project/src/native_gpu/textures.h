// bf2mc native GPU - Xenos texture fetch constants -> host texture data (untiling, endian swap,
// format conversion). Layout rules follow rex/graphics/pipeline/texture/util.h (Xenia).

#pragma once

#include <dxgiformat.h>

#include <cstdint>
#include <vector>

namespace bf2n {

struct TexFetch {
  uint32_t d[6] = {};
  uint32_t type() const { return d[0] & 3; }
  uint32_t sign(uint32_t c) const { return (d[0] >> (2 + 2 * c)) & 3; }
  uint32_t clamp(uint32_t axis) const { return (d[0] >> (10 + 3 * axis)) & 7; }
  uint32_t pitch_texels() const { return ((d[0] >> 22) & 0x1FF) << 5; }
  bool tiled() const { return d[0] >> 31; }
  uint32_t format() const { return d[1] & 0x3F; }
  uint32_t endian() const { return (d[1] >> 6) & 3; }
  bool stacked() const { return (d[1] >> 10) & 1; }
  uint32_t base_address() const { return d[1] & 0xFFFFF000u; }
  uint32_t swizzle() const { return (d[3] >> 1) & 0xFFF; }
  uint32_t mag_filter() const { return (d[3] >> 19) & 3; }
  uint32_t min_filter() const { return (d[3] >> 21) & 3; }
  uint32_t mip_filter() const { return (d[3] >> 23) & 3; }
  uint32_t aniso_filter() const { return (d[3] >> 25) & 7; }
  uint32_t mip_min_level() const { return (d[4] >> 2) & 0xF; }
  uint32_t mip_max_level() const { return (d[4] >> 6) & 0xF; }
  float lod_bias() const { return float(int32_t(d[4] << 10) >> 22) / 32.0f; }
  uint32_t border_color() const { return d[5] & 3; }
  uint32_t dimension() const { return (d[5] >> 9) & 3; }  // 0 1D, 1 2D/stacked, 2 3D, 3 cube
  bool packed_mips() const { return (d[5] >> 11) & 1; }
  uint32_t mip_address() const { return d[5] & 0xFFFFF000u; }
  uint32_t width() const;   // in texels
  uint32_t height() const;
  uint32_t depth() const;   // 3D depth, stacked/cube array size
};

struct HostTexture {
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  uint32_t width = 1, height = 1, depth = 1;  // depth = 3D depth or array size
  uint32_t levels = 1;
  bool is_3d = false, is_cube = false;
  // Subresources in D3D11 order (array slice major, level minor; 3D: one per level).
  struct Sub {
    std::vector<uint8_t> data;
    uint32_t row_pitch = 0, slice_pitch = 0;
  };
  std::vector<Sub> subs;
};

// Host format for the guest format (and whether all components are signed).
DXGI_FORMAT HostTextureFormat(uint32_t guest_format, bool is_signed);

// Decodes the texture referenced by `fetch` from guest physical memory. Returns false for
// unsupported formats / empty textures.
bool DecodeTexture(const uint8_t* physical_membase, const TexFetch& fetch, HostTexture& out);

// Guest physical memory the texture occupies: base level and, if any, the mip chain.
struct TextureMemory {
  uint32_t base = 0, base_size = 0, mip = 0, mip_size = 0;
};
TextureMemory TextureMemoryRanges(const TexFetch& fetch);

// Hash of the guest memory the texture occupies (to detect CPU updates).
uint64_t HashTextureMemory(const uint8_t* physical_membase, const TexFetch& fetch);

}  // namespace bf2n
