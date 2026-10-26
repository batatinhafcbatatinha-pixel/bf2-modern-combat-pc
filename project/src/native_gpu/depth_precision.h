#pragma once
#include <bit>
#include <cstdint>

namespace bf2n {
// 20e4 round-to-nearest-even, matching the SDK's PreClampedDepthTo20e4 /
// Depth20e4To32 conversion, keeping decoded guest depths in the 0..1 range.
inline float QuantizeDepth20e4(float value) {
  if (!(value>0.0f)) return 0.0f;
  if (value>=1.0f) return 1.0f;
  uint32_t bits=std::bit_cast<uint32_t>(value);
  if (bits<0x38800000u) {
    uint32_t shift=113u-(bits>>23);
    if (shift>24) shift=24;
    uint32_t biased=((bits&0x7fffffu)|0x800000u)>>shift;
    uint32_t mantissa=(biased+3u+((biased>>3)&1u))>>3;
    return float(mantissa)*0x1p-34f;
  }
  return std::bit_cast<float>((bits+3u+((bits>>3)&1u))&~7u);
}

inline constexpr char kDepthPrecisionHlsl[]=R"(
float xdepth20e4(float value) {
  uint bits=asuint(saturate(value));
  if(bits<0x38800000u) {
    uint shift=min(113u-(bits>>23),24u);
    uint biased=((bits&0x7fffffu)|0x800000u)>>shift;
    uint mantissa=(biased+3u+((biased>>3)&1u))>>3;
    return float(mantissa)*5.82076609134674072265625e-11;
  }
  return asfloat((bits+3u+((bits>>3)&1u))&~7u);
}
)";
}
