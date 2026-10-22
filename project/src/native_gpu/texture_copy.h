#pragma once
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

namespace bf2n {
// Exact equivalent of byte reads at ((address+i)^endian_xor)&physical_mask.
// Work a row at a time for linear textures instead of calling ReadBlock per texel.
inline void CopyTextureRow(const uint8_t* memory, uint32_t address, uint32_t bytes,
                           uint32_t endian_xor, uint8_t* output) {
  constexpr uint32_t mask = 0x1fffffffu;
  while (bytes) {
    address &= mask;
    if (!endian_xor) {
      uint32_t n = bytes < mask + 1 - address ? bytes : mask + 1 - address;
      std::memcpy(output, memory + address, n);
      address += n; output += n; bytes -= n;
      continue;
    }
    if ((address & 3) || bytes < 16 || address > mask - 15) {
      *output++ = memory[(address++ ^ endian_xor) & mask];
      --bytes;
      continue;
    }
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(memory + address));
    if (endian_xor & 1) v = _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8));
    if (endian_xor & 2) v = _mm_or_si128(_mm_slli_epi32(v, 16), _mm_srli_epi32(v, 16));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(output), v);
    address += 16; output += 16; bytes -= 16;
  }
}
}
