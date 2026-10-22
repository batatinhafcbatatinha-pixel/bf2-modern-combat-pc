// bf2mc native GPU - guest texture decoding (see textures.h).

#include "textures.h"
#include "texture_copy.h"

#include <xxhash.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bf2n {
namespace {

enum Conv : uint8_t {
  kNone,  // unsupported
  kCopy,
  k565,
  k1555,
  k655,
  k4444,
  k10_11_11,
  k11_11_10,
  k24_8,
  k24_8f,
  kCtx1,
  kDxt3a,
};

struct FormatDesc {
  uint8_t bw, bh, bpb;
  Conv conv;
  DXGI_FORMAT host;
  DXGI_FORMAT host_signed;
};

constexpr DXGI_FORMAT U = DXGI_FORMAT_UNKNOWN;

const FormatDesc kFormats[64] = {
    /* 0 1_REVERSE */ {1, 1, 1, kNone, U, U},
    /* 1 1 */ {1, 1, 1, kNone, U, U},
    /* 2 8 */ {1, 1, 1, kCopy, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM},
    /* 3 1_5_5_5 */ {1, 1, 2, k1555, DXGI_FORMAT_R8G8B8A8_UNORM, U},
    /* 4 5_6_5 */ {1, 1, 2, k565, DXGI_FORMAT_R8G8B8A8_UNORM, U},
    /* 5 6_5_5 */ {1, 1, 2, k655, DXGI_FORMAT_R8G8B8A8_UNORM, U},
    /* 6 8_8_8_8 */ {1, 1, 4, kCopy, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM},
    /* 7 2_10_10_10 */ {1, 1, 4, kCopy, DXGI_FORMAT_R10G10B10A2_UNORM, U},
    /* 8 8_A */ {1, 1, 1, kCopy, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM},
    /* 9 8_B */ {1, 1, 1, kCopy, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM},
    /* 10 8_8 */ {1, 1, 2, kCopy, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM},
    /* 11 Cr_Y1_Cb_Y0_REP */ {2, 1, 4, kNone, U, U},
    /* 12 Y1_Cr_Y0_Cb_REP */ {2, 1, 4, kNone, U, U},
    /* 13 16_16_EDRAM */ {1, 1, 4, kCopy, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM},
    /* 14 8_8_8_8_A */ {1, 1, 4, kCopy, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM},
    /* 15 4_4_4_4 */ {1, 1, 2, k4444, DXGI_FORMAT_R8G8B8A8_UNORM, U},
    /* 16 10_11_11 */ {1, 1, 4, k10_11_11, DXGI_FORMAT_R16G16B16A16_UNORM, U},
    /* 17 11_11_10 */ {1, 1, 4, k11_11_10, DXGI_FORMAT_R16G16B16A16_UNORM, U},
    /* 18 DXT1 */ {4, 4, 8, kCopy, DXGI_FORMAT_BC1_UNORM, U},
    /* 19 DXT2_3 */ {4, 4, 16, kCopy, DXGI_FORMAT_BC2_UNORM, U},
    /* 20 DXT4_5 */ {4, 4, 16, kCopy, DXGI_FORMAT_BC3_UNORM, U},
    /* 21 16_16_16_16_EDRAM */ {1, 1, 8, kCopy, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM},
    /* 22 24_8 */ {1, 1, 4, k24_8, DXGI_FORMAT_R32_FLOAT, U},
    /* 23 24_8_FLOAT */ {1, 1, 4, k24_8f, DXGI_FORMAT_R32_FLOAT, U},
    /* 24 16 */ {1, 1, 2, kCopy, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM},
    /* 25 16_16 */ {1, 1, 4, kCopy, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM},
    /* 26 16_16_16_16 */ {1, 1, 8, kCopy, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R16G16B16A16_SNORM},
    /* 27 16_EXPAND */ {1, 1, 2, kCopy, DXGI_FORMAT_R16_FLOAT, U},
    /* 28 16_16_EXPAND */ {1, 1, 4, kCopy, DXGI_FORMAT_R16G16_FLOAT, U},
    /* 29 16_16_16_16_EXPAND */ {1, 1, 8, kCopy, DXGI_FORMAT_R16G16B16A16_FLOAT, U},
    /* 30 16_FLOAT */ {1, 1, 2, kCopy, DXGI_FORMAT_R16_FLOAT, U},
    /* 31 16_16_FLOAT */ {1, 1, 4, kCopy, DXGI_FORMAT_R16G16_FLOAT, U},
    /* 32 16_16_16_16_FLOAT */ {1, 1, 8, kCopy, DXGI_FORMAT_R16G16B16A16_FLOAT, U},
    /* 33 32 */ {1, 1, 4, kCopy, DXGI_FORMAT_R32_FLOAT, U},
    /* 34 32_32 */ {1, 1, 8, kCopy, DXGI_FORMAT_R32G32_FLOAT, U},
    /* 35 32_32_32_32 */ {1, 1, 16, kCopy, DXGI_FORMAT_R32G32B32A32_FLOAT, U},
    /* 36 32_FLOAT */ {1, 1, 4, kCopy, DXGI_FORMAT_R32_FLOAT, U},
    /* 37 32_32_FLOAT */ {1, 1, 8, kCopy, DXGI_FORMAT_R32G32_FLOAT, U},
    /* 38 32_32_32_32_FLOAT */ {1, 1, 16, kCopy, DXGI_FORMAT_R32G32B32A32_FLOAT, U},
    /* 39 32_AS_8 */ {4, 1, 4, kCopy, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM},
    /* 40 32_AS_8_8 */ {2, 1, 4, kCopy, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM},
    /* 41 16_MPEG */ {1, 1, 2, kCopy, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM},
    /* 42 16_16_MPEG */ {1, 1, 4, kCopy, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM},
    /* 43 8_INTERLACED */ {1, 1, 1, kCopy, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM},
    /* 44 32_AS_8_INTERLACED */ {4, 1, 4, kCopy, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_SNORM},
    /* 45 32_AS_8_8_INTERLACED */ {2, 1, 4, kCopy, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8_SNORM},
    /* 46 16_INTERLACED */ {1, 1, 2, kCopy, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM},
    /* 47 16_MPEG_INTERLACED */ {1, 1, 2, kCopy, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16_SNORM},
    /* 48 16_16_MPEG_INTERLACED */ {1, 1, 4, kCopy, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_R16G16_SNORM},
    /* 49 DXN */ {4, 4, 16, kCopy, DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_SNORM},
    /* 50 8_8_8_8_AS_16_16_16_16 */ {1, 1, 4, kCopy, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_SNORM},
    /* 51 DXT1_AS_16_16_16_16 */ {4, 4, 8, kCopy, DXGI_FORMAT_BC1_UNORM, U},
    /* 52 DXT2_3_AS_16_16_16_16 */ {4, 4, 16, kCopy, DXGI_FORMAT_BC2_UNORM, U},
    /* 53 DXT4_5_AS_16_16_16_16 */ {4, 4, 16, kCopy, DXGI_FORMAT_BC3_UNORM, U},
    /* 54 2_10_10_10_AS_16_16_16_16 */ {1, 1, 4, kCopy, DXGI_FORMAT_R10G10B10A2_UNORM, U},
    /* 55 10_11_11_AS_16_16_16_16 */ {1, 1, 4, k10_11_11, DXGI_FORMAT_R16G16B16A16_UNORM, U},
    /* 56 11_11_10_AS_16_16_16_16 */ {1, 1, 4, k11_11_10, DXGI_FORMAT_R16G16B16A16_UNORM, U},
    /* 57 32_32_32_FLOAT */ {1, 1, 12, kNone, U, U},
    /* 58 DXT3A */ {4, 4, 8, kDxt3a, DXGI_FORMAT_R8_UNORM, U},
    /* 59 DXT5A */ {4, 4, 8, kCopy, DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_SNORM},
    /* 60 CTX1 */ {4, 4, 8, kCtx1, DXGI_FORMAT_R8G8_UNORM, U},
    /* 61 DXT3A_AS_1_1_1_1 */ {4, 4, 8, kNone, U, U},
    /* 62 8_8_8_8_GAMMA_EDRAM */ {1, 1, 4, kCopy, DXGI_FORMAT_R8G8B8A8_UNORM, U},
    /* 63 2_10_10_10_FLOAT_EDRAM */ {1, 1, 4, kNone, U, U},
};

uint32_t Log2Floor(uint32_t v) {
  uint32_t r = 0;
  while (v >>= 1) ++r;
  return r;
}
uint32_t Log2Ceil(uint32_t v) { return v <= 1 ? 0 : Log2Floor(v - 1) + 1; }
uint32_t NextPow2(uint32_t v) { return v <= 1 ? 1 : 1u << Log2Ceil(v); }
uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bpb_log2) {
  pitch = Align(pitch, 32);
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bpb_log2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

int32_t TiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height,
                      uint32_t bpb_log2) {
  pitch = Align(pitch, 32);
  height = Align(height, 32);
  int32_t macro_outer = ((y >> 4) + (z >> 2) * int32_t(height >> 4)) * int32_t(pitch >> 5);
  int32_t macro = ((((x >> 5) + macro_outer) << (bpb_log2 + 6)) & 0xFFFFFFF) << 1;
  int32_t micro = (((x & 7) + ((y & 6) << 2)) << (bpb_log2 + 6)) >> 6;
  int32_t offset_outer = ((y >> 3) + (z >> 2)) & 1;
  int32_t offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
  int32_t offset2 = ((macro + (micro & ~15)) << 1) + (micro & 15) +
                    ((z & 3) << (bpb_log2 + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

// Offset of a packed mip inside the mip tail, in blocks (GetPackedMipOffset).
bool PackedMipOffset(uint32_t width, uint32_t height, uint32_t depth, const FormatDesc& f,
                     uint32_t mip, uint32_t& xb, uint32_t& yb, uint32_t& zb) {
  uint32_t lw = Log2Ceil(width), lh = Log2Ceil(height);
  uint32_t ls = std::min(lw, lh);
  if (ls > 4 + mip) {
    xb = yb = zb = 0;
    return false;
  }
  uint32_t base = ls > 4 ? ls - 4 : 0;
  uint32_t pm = mip - base;
  if (pm < 3) {
    if (lw > lh) {
      xb = 0;
      yb = 16 >> pm;
    } else {
      xb = 16 >> pm;
      yb = 0;
    }
    zb = 0;
  } else {
    uint32_t offset;
    if (lw > lh) {
      offset = (1u << (lw - base)) >> (pm - 2);
      xb = offset;
      yb = 0;
    } else {
      xb = 0;
      offset = (1u << (lh - base)) >> (pm - 2);
      yb = offset;
    }
    if (offset < 4) {
      uint32_t ld = Log2Ceil(depth);
      zb = ld > 1 + pm ? (ld - pm) * 4 : 4;
    } else {
      zb = 0;
    }
  }
  xb /= f.bw;
  yb /= f.bh;
  return true;
}

struct LevelLayout {
  uint32_t row_pitch_blocks = 0;  // tile-aligned
  uint32_t row_pitch_bytes = 0;
  uint32_t slice_rows = 0;  // block rows per z slice
  uint32_t array_stride = 0;
};

LevelLayout ComputeLevel(const FormatDesc& f, bool is_base, uint32_t level, uint32_t pitch_texels,
                         uint32_t w, uint32_t h, uint32_t d, bool is_3d, bool tiled, bool is_1d) {
  LevelLayout l;
  uint32_t row_texels, rows_texels;
  if (is_base) {
    row_texels = pitch_texels;
    rows_texels = h;
  } else {
    row_texels = std::max(NextPow2(w) >> level, 1u);
    rows_texels = std::max(NextPow2(h) >> level, 1u);
  }
  l.row_pitch_blocks = Align(Align(row_texels, f.bw) / f.bw, 32);
  l.row_pitch_bytes = l.row_pitch_blocks * f.bpb;
  if (!tiled && !is_base) l.row_pitch_bytes = Align(l.row_pitch_bytes, 256);
  l.slice_rows = is_1d ? 1 : Align(Align(rows_texels, f.bh) / f.bh, 32);
  uint32_t stride = l.row_pitch_bytes * l.slice_rows;
  if (is_3d) stride *= Align(d, 4);
  l.array_stride = Align(stride, 4096);
  return l;
}

struct Layout {
  uint32_t levels = 1;
  uint32_t packed_level = ~0u;
  LevelLayout base;
  LevelLayout mips[16];
  uint32_t mip_offsets[16] = {};
  uint32_t mips_extent = 0;
};

Layout ComputeLayout(const FormatDesc& f, const TexFetch& t, uint32_t w, uint32_t h, uint32_t d,
                     uint32_t array_size, uint32_t levels) {
  Layout L;
  bool is_3d = t.dimension() == 2, is_1d = t.dimension() == 0;
  L.levels = levels;
  if (t.packed_mips()) {
    uint32_t ls = Log2Ceil(std::min(w, h));
    L.packed_level = ls > 4 ? ls - 4 : 0;
  }
  L.base = ComputeLevel(f, true, 0, t.pitch_texels(), w, h, d, is_3d, t.tiled(), is_1d);
  uint32_t last_stored = std::min(levels - 1, L.packed_level);
  uint32_t offset = 0;
  // Mip storage: levels 1..last_stored, or the level-0-shaped mip tail when packed_level == 0.
  uint32_t first = L.packed_level == 0 ? 0 : 1;
  if (levels > 1) {
    for (uint32_t lv = first; lv <= std::max(last_stored, first); ++lv) {
      L.mips[lv] = ComputeLevel(f, false, lv, 0, w, h, d, is_3d, t.tiled(), is_1d);
      L.mip_offsets[lv] = offset;
      offset += L.mips[lv].array_stride * array_size;
      if (L.packed_level == 0) break;
    }
  }
  L.mips_extent = offset;
  return L;
}

inline uint32_t SwapXor(uint32_t endian) {
  static const uint32_t k[4] = {0, 1, 3, 2};
  return k[endian & 3];
}

// Copies one block from guest memory (with the fetch constant's endian swap).
inline void ReadBlock(const uint8_t* mem, uint32_t addr, uint32_t bpb, uint32_t sx, uint8_t* dst) {
  if (sx == 0) {
    std::memcpy(dst, mem + (addr & 0x1FFFFFFF), bpb);
    return;
  }
  for (uint32_t i = 0; i < bpb; ++i) dst[i] = mem[((addr + i) ^ sx) & 0x1FFFFFFF];
}

inline uint8_t Expand(uint32_t v, uint32_t bits) {
  return uint8_t((v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1));
}

void ConvertBlock(Conv c, const uint8_t* src, uint8_t* dst) {
  switch (c) {
    case k565: {
      uint32_t v = src[0] | (src[1] << 8);
      dst[0] = Expand(v & 31, 5);
      dst[1] = Expand((v >> 5) & 63, 6);
      dst[2] = Expand((v >> 11) & 31, 5);
      dst[3] = 255;
      break;
    }
    case k655: {
      uint32_t v = src[0] | (src[1] << 8);
      dst[0] = Expand(v & 63, 6);
      dst[1] = Expand((v >> 6) & 31, 5);
      dst[2] = Expand((v >> 11) & 31, 5);
      dst[3] = 255;
      break;
    }
    case k1555: {
      uint32_t v = src[0] | (src[1] << 8);
      dst[0] = Expand(v & 31, 5);
      dst[1] = Expand((v >> 5) & 31, 5);
      dst[2] = Expand((v >> 10) & 31, 5);
      dst[3] = (v >> 15) ? 255 : 0;
      break;
    }
    case k4444: {
      uint32_t v = src[0] | (src[1] << 8);
      dst[0] = Expand(v & 15, 4);
      dst[1] = Expand((v >> 4) & 15, 4);
      dst[2] = Expand((v >> 8) & 15, 4);
      dst[3] = Expand((v >> 12) & 15, 4);
      break;
    }
    case k10_11_11:
    case k11_11_10: {
      uint32_t v;
      std::memcpy(&v, src, 4);
      uint32_t c0, c1, c2, w0, w1, w2;
      if (c == k10_11_11) {
        w0 = 11; w1 = 11; w2 = 10;
      } else {
        w0 = 10; w1 = 11; w2 = 11;
      }
      c0 = v & ((1u << w0) - 1);
      c1 = (v >> w0) & ((1u << w1) - 1);
      c2 = (v >> (w0 + w1)) & ((1u << w2) - 1);
      uint16_t o[4] = {uint16_t(c0 * 65535 / ((1u << w0) - 1)), uint16_t(c1 * 65535 / ((1u << w1) - 1)),
                       uint16_t(c2 * 65535 / ((1u << w2) - 1)), 65535};
      std::memcpy(dst, o, 8);
      break;
    }
    case k24_8: {
      uint32_t v;
      std::memcpy(&v, src, 4);
      float f = float(v >> 8) / 16777215.0f;
      std::memcpy(dst, &f, 4);
      break;
    }
    case k24_8f: {
      uint32_t v;
      std::memcpy(&v, src, 4);
      uint32_t f24 = v >> 8;  // 20e4
      uint32_t mant = f24 & 0xFFFFF, exp = f24 >> 20;
      float f;
      if (exp == 0) {
        f = std::ldexp(float(mant), -20 - 14);
      } else {
        f = std::ldexp(1.0f + float(mant) / 1048576.0f, int(exp) - 15);
      }
      std::memcpy(dst, &f, 4);
      break;
    }
    default:
      break;
  }
}

// CTX1: two 8:8 endpoints + 2-bit indices -> R8G8 4x4 block (dst row pitch in bytes).
void DecodeCtx1(const uint8_t* b, uint8_t* dst, uint32_t dst_pitch) {
  uint8_t c[4][2];
  // Endpoints after the endian swap: g0, r0, g1, r1 (red in the high byte of each 16-bit pair).
  c[0][0] = b[1]; c[0][1] = b[0];
  c[1][0] = b[3]; c[1][1] = b[2];
  for (int k = 0; k < 2; ++k) {
    c[2][k] = uint8_t((2 * c[0][k] + c[1][k] + 1) / 3);
    c[3][k] = uint8_t((c[0][k] + 2 * c[1][k] + 1) / 3);
  }
  uint32_t idx = b[4] | (b[5] << 8) | (b[6] << 16) | (uint32_t(b[7]) << 24);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      uint32_t i = (idx >> (2 * (y * 4 + x))) & 3;
      dst[y * dst_pitch + x * 2] = c[i][0];
      dst[y * dst_pitch + x * 2 + 1] = c[i][1];
    }
  }
}

void DecodeDxt3a(const uint8_t* b, uint8_t* dst, uint32_t dst_pitch) {
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      int n = y * 4 + x;
      uint32_t a = (b[n >> 1] >> ((n & 1) * 4)) & 15;
      dst[y * dst_pitch + x] = uint8_t(a * 17);
    }
  }
}

}  // namespace

uint32_t TexFetch::width() const {
  uint32_t dim = dimension();
  if (dim == 0) return (d[2] & 0xFFFFFF) + 1;
  if (dim == 2) return (d[2] & 0x7FF) + 1;
  return (d[2] & 0x1FFF) + 1;
}
uint32_t TexFetch::height() const {
  uint32_t dim = dimension();
  if (dim == 0) return 1;
  if (dim == 2) return ((d[2] >> 11) & 0x7FF) + 1;
  return ((d[2] >> 13) & 0x1FFF) + 1;
}
uint32_t TexFetch::depth() const {
  uint32_t dim = dimension();
  if (dim == 2) return ((d[2] >> 22) & 0x3FF) + 1;
  if (dim == 3) return 6;
  if (dim == 1 && stacked()) return ((d[2] >> 26) & 0x3F) + 1;
  return 1;
}

DXGI_FORMAT HostTextureFormat(uint32_t guest_format, bool is_signed) {
  const FormatDesc& f = kFormats[guest_format & 63];
  if (is_signed && f.host_signed != U) return f.host_signed;
  return f.host;
}

static void LevelCount(const TexFetch& t, uint32_t w, uint32_t h, uint32_t d, uint32_t& levels) {
  uint32_t longest = std::max(w, h);
  if (t.dimension() == 2) longest = std::max(longest, d);
  uint32_t size_max = Log2Floor(longest);
  levels = 1;
  if (t.mip_address() != 0) {
    uint32_t mx = std::min(t.mip_max_level(), size_max);
    levels = mx + 1;
  }
}

bool DecodeTexture(const uint8_t* mem, const TexFetch& t, HostTexture& out) {
  const FormatDesc& f = kFormats[t.format()];
  if (f.conv == kNone) return false;
  bool all_signed = t.sign(0) == 1 && t.sign(1) == 1 && t.sign(2) == 1 && t.sign(3) == 1;
  out.format = HostTextureFormat(t.format(), all_signed);
  if (out.format == U) return false;

  uint32_t w = t.width(), h = t.height(), d = t.depth();
  bool is_3d = t.dimension() == 2;
  uint32_t array_size = is_3d ? 1 : d;
  uint32_t vol_depth = is_3d ? d : 1;
  uint32_t levels;
  LevelCount(t, w, h, d, levels);
  if (t.base_address() == 0) return false;

  out.width = w;
  out.height = h;
  out.depth = d;
  out.is_3d = is_3d;
  out.is_cube = t.dimension() == 3;
  out.levels = levels;
  out.subs.clear();
  out.subs.resize(size_t(array_size) * levels);

  Layout L = ComputeLayout(f, t, w, h, vol_depth, array_size, levels);
  uint32_t sx = SwapXor(t.endian());
  uint32_t bpb_log2 = Log2Floor(f.bpb);
  bool compressed_out = f.conv == kCopy && f.bw == 4;
  uint32_t out_bpp;  // bytes per output texel (or per block for BC)
  switch (f.conv) {
    case k565: case k655: case k1555: case k4444: out_bpp = 4; break;
    case k10_11_11: case k11_11_10: out_bpp = 8; break;
    case k24_8: case k24_8f: out_bpp = 4; break;
    case kCtx1: out_bpp = 2; break;
    case kDxt3a: out_bpp = 1; break;
    default: out_bpp = compressed_out ? f.bpb : (f.bpb / (f.bw * f.bh)); break;
  }

  std::vector<uint8_t> block(16);
  for (uint32_t level = 0; level < levels; ++level) {
    uint32_t lw = std::max(w >> level, 1u), lh = std::max(h >> level, 1u);
    uint32_t ld = std::max(vol_depth >> level, 1u);
    uint32_t bw = (lw + f.bw - 1) / f.bw, bh = (lh + f.bh - 1) / f.bh;
    // Location of the level's storage.
    uint32_t storage_level = level;
    bool packed = level >= L.packed_level;
    uint32_t px = 0, py = 0, pz = 0;
    if (packed) {
      storage_level = L.packed_level;
      PackedMipOffset(w, h, vol_depth, f, level, px, py, pz);
    }
    const LevelLayout& ll = (storage_level == 0 && level == 0) ? L.base
                            : (level == 0 ? L.base : L.mips[storage_level]);
    uint32_t level_base;
    if (level == 0) {
      level_base = t.base_address();
    } else {
      if (!t.mip_address()) break;
      level_base = t.mip_address() + L.mip_offsets[storage_level];
    }
    for (uint32_t slice = 0; slice < array_size; ++slice) {
      HostTexture::Sub& sub = out.subs[size_t(slice) * levels + level];
      uint32_t row_pitch;
      if (compressed_out) row_pitch = bw * f.bpb;
      else if (f.conv == kCtx1 || f.conv == kDxt3a) row_pitch = lw * out_bpp;
      else row_pitch = bw * f.bw * out_bpp;
      uint32_t out_rows = (compressed_out ? bh : (f.conv == kCtx1 || f.conv == kDxt3a ? bh * 4 : bh));
      sub.row_pitch = row_pitch;
      sub.slice_pitch = row_pitch * out_rows;
      sub.data.assign(size_t(sub.slice_pitch) * ld + 64, 0);
      uint32_t slice_base = level_base + slice * ll.array_stride;
      if (!t.tiled() && f.conv == kCopy) {
        for (uint32_t z = 0; z < ld; ++z) {
          for (uint32_t y = 0; y < bh; ++y) {
            uint32_t address = slice_base + (z + pz) * ll.row_pitch_bytes * ll.slice_rows +
                               (y + py) * ll.row_pitch_bytes + px * f.bpb;
            CopyTextureRow(mem, address, bw * f.bpb, sx,
                sub.data.data() + size_t(z) * sub.slice_pitch + size_t(y) * row_pitch);
          }
        }
        continue;
      }
      for (uint32_t z = 0; z < ld; ++z) {
        for (uint32_t by = 0; by < bh; ++by) {
          for (uint32_t bx = 0; bx < bw; ++bx) {
            uint32_t x = bx + px, y = by + py, zz = z + pz;
            uint32_t offset;
            if (t.tiled()) {
              offset = is_3d ? uint32_t(TiledOffset3D(x, y, zz, ll.row_pitch_blocks, ll.slice_rows,
                                                      bpb_log2))
                             : uint32_t(TiledOffset2D(x, y, ll.row_pitch_blocks, bpb_log2));
            } else {
              offset = zz * ll.row_pitch_bytes * ll.slice_rows + y * ll.row_pitch_bytes + x * f.bpb;
            }
            ReadBlock(mem, slice_base + offset, f.bpb, sx, block.data());
            uint8_t* dz = sub.data.data() + size_t(z) * sub.slice_pitch;
            if (compressed_out) {
              std::memcpy(dz + by * row_pitch + bx * f.bpb, block.data(), f.bpb);
            } else if (f.conv == kCtx1) {
              if (bx * 4 < lw && by * 4 < lh) {
                uint8_t tmp[4 * 4 * 2];
                DecodeCtx1(block.data(), tmp, 8);
                for (uint32_t yy = 0; yy < 4 && by * 4 + yy < lh; ++yy)
                  for (uint32_t xx = 0; xx < 4 && bx * 4 + xx < lw; ++xx)
                    std::memcpy(dz + (by * 4 + yy) * row_pitch + (bx * 4 + xx) * 2, tmp + yy * 8 + xx * 2, 2);
              }
            } else if (f.conv == kDxt3a) {
              uint8_t tmp[16];
              DecodeDxt3a(block.data(), tmp, 4);
              for (uint32_t yy = 0; yy < 4 && by * 4 + yy < lh; ++yy)
                for (uint32_t xx = 0; xx < 4 && bx * 4 + xx < lw; ++xx)
                  dz[(by * 4 + yy) * row_pitch + bx * 4 + xx] = tmp[yy * 4 + xx];
            } else if (f.conv == kCopy) {
              std::memcpy(dz + by * row_pitch + bx * f.bpb, block.data(), f.bpb);
            } else {
              ConvertBlock(f.conv, block.data(), dz + by * row_pitch + bx * out_bpp);
            }
          }
        }
      }
    }
  }
  // Drop levels that could not be located.
  return !out.subs.empty() && !out.subs[0].data.empty();
}

uint64_t HashTextureMemory(const uint8_t* mem, const TexFetch& t) {
  const FormatDesc& f = kFormats[t.format()];
  uint32_t w = t.width(), h = t.height(), d = t.depth();
  bool is_3d = t.dimension() == 2;
  uint32_t array_size = is_3d ? 1 : d;
  uint32_t levels;
  LevelCount(t, w, h, d, levels);
  Layout L = ComputeLayout(f, t, w, h, is_3d ? d : 1, array_size, levels);
  // Base level: the actually referenced rows (avoid reading past small allocations).
  uint32_t bh = (h + f.bh - 1) / f.bh;
  uint32_t base_bytes;
  if (t.tiled()) {
    base_bytes = L.base.array_stride * (array_size - 1) +
                 std::min(L.base.array_stride, L.base.row_pitch_bytes * Align(bh, 32));
  } else {
    base_bytes = L.base.array_stride * (array_size - 1) + L.base.row_pitch_bytes * bh;
  }
  if (L.packed_level == 0) base_bytes = std::max(base_bytes, L.base.array_stride);
  base_bytes = std::min<uint32_t>(base_bytes, 64u << 20);
  uint32_t base = t.base_address() & 0x1FFFFFFF;
  uint64_t hash = XXH3_64bits(mem + base, std::min<uint32_t>(base_bytes, 0x20000000u - base));
  if (levels > 1 && t.mip_address()) {
    uint32_t mip = t.mip_address() & 0x1FFFFFFF;
    uint32_t extent = std::min<uint32_t>(L.mips_extent, 0x20000000u - mip);
    hash ^= XXH3_64bits(mem + mip, std::min<uint32_t>(extent, 64u << 20)) * 0x9E3779B97F4A7C15ull;
  }
  return hash;
}

}  // namespace bf2n
