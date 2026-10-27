#pragma once
#include <bit>
#include <cstdint>
namespace native_host {
inline uint16_t ByteSwap16(uint16_t value) { return std::byteswap(value); }
inline uint32_t ByteSwap32(uint32_t value) { return std::byteswap(value); }
inline uint64_t ByteSwap64(uint64_t value) { return std::byteswap(value); }
}
