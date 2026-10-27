#include "../platform/native_byteorder.h"
// Layout of the XMA decoder context shared between the game and bf2audio::XmaEngine: 16
// big-endian dwords of bit fields (bit positions are LSB-first within each dword).
#pragma once

#include <cstdint>
#include <cstdlib>

namespace bf2audio {

inline uint32_t XmaBe32(const uint8_t* p) { return native_host::ByteSwap32(*reinterpret_cast<const uint32_t*>(p)); }
inline void XmaStoreBe32(uint8_t* p, uint32_t v) { *reinterpret_cast<uint32_t*>(p) = native_host::ByteSwap32(v); }

struct Field {
  uint8_t word, lo, bits;
};
constexpr Field kIn0Packets{0, 0, 12}, kLoopCount{0, 12, 8}, kIn0Valid{0, 20, 1}, kIn1Valid{0, 21, 1},
    kOutBlocks{0, 22, 5}, kOutWrite{0, 27, 5};
constexpr Field kIn1Packets{1, 0, 12}, kLoopSubframeEnd{1, 12, 2}, kLoopSubframeSkip{1, 17, 3},
    kSubframeDecodeCount{1, 20, 4}, kOutPadding{1, 24, 3}, kSampleRate{1, 27, 2}, kStereo{1, 29, 1},
    kOutValid{1, 31, 1};
constexpr Field kInReadOffset{2, 0, 26}, kErrorStatus{2, 26, 5};
constexpr Field kLoopStart{3, 0, 26};
constexpr Field kLoopEnd{4, 0, 26}, kCurrentBuffer{4, 31, 1};
constexpr Field kOutRead{9, 0, 5};
constexpr uint32_t kIn0Ptr = 5, kIn1Ptr = 6, kOutPtr = 7;

struct Ctx {
  uint32_t d[16];
  void Load(const uint8_t* p) {
    for (int i = 0; i < 16; i++) d[i] = XmaBe32(p + i * 4);
  }
  void Store(uint8_t* p) const {
    for (int i = 0; i < 16; i++) XmaStoreBe32(p + i * 4, d[i]);
  }
  uint32_t operator[](Field f) const { return (d[f.word] >> f.lo) & ((1u << f.bits) - 1); }
  void set(Field f, uint32_t v) {
    uint32_t m = ((1u << f.bits) - 1) << f.lo;
    d[f.word] = (d[f.word] & ~m) | ((v << f.lo) & m);
  }
  bool InputValid(uint32_t b) const { return (*this)[b ? kIn1Valid : kIn0Valid] != 0; }
  bool AnyInputValid() const { return InputValid(0) || InputValid(1); }
  uint32_t InputAddress(uint32_t b) const { return d[b ? kIn1Ptr : kIn0Ptr]; }
  uint32_t InputPackets(uint32_t b) const { return (*this)[b ? kIn1Packets : kIn0Packets]; }
  uint32_t current() const { return (*this)[kCurrentBuffer]; }
};

}  // namespace bf2audio
