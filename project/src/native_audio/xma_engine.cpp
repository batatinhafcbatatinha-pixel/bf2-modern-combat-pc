#include "../platform/native_byteorder.h"
// Native XMA decoding. See xma_engine.h.
//
// Bitstream layout and the decoder's buffer semantics (packets of 2048 bytes with a 32-bit header,
// frames with a 15-bit length that may straddle packets, output in 256-byte blocks, loop points
// with subframe precision, the 192-sample start padding FFmpeg emits) follow the XMA research in
// Xenia's apu/xma_context (BSD); the implementation here is our own and has no register file.

#include "xma_engine.h"
#include "xma_fields.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
}

namespace bf2audio {

namespace {

constexpr uint32_t kBytesPerPacket = 2048;
constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8;
constexpr uint32_t kBitsPerPacketHeader = 32;
constexpr uint32_t kBytesPerPacketData = kBytesPerPacket - 4;
constexpr uint32_t kBitsPerFrameHeader = 15;
constexpr uint32_t kMaxFrameLength = 0x7FFF;
constexpr uint32_t kMaxFrameSizeInBits = 0x4000 - kBitsPerPacketHeader;
constexpr uint32_t kSamplesPerFrame = 512;
constexpr uint32_t kBytesPerFrameChannel = kSamplesPerFrame * 2;
constexpr uint32_t kOutputBlockBytes = 256;
constexpr uint32_t kBlocksPerFrameChannel = kBytesPerFrameChannel / kOutputBlockBytes;  // 4
// FFmpeg's xmaframes decoder outputs the bitstream's start padding instead of dropping it, so its
// sample numbering runs this far ahead of the guest's (loop points included).
constexpr uint32_t kDecoderStartPadding = 192;
constexpr int kSampleRates[4] = {24000, 32000, 44100, 48000};

// Big-endian bit reader over a byte buffer (n <= 32, reads byte by byte: never past `size`).
uint32_t PeekBits(const uint8_t* buf, size_t size_bits, size_t offset, uint32_t n) {
  uint64_t v = 0;
  for (uint32_t i = 0; i < n; i++) {
    size_t bit = offset + i;
    uint32_t b = bit < size_bits ? (buf[bit >> 3] >> (7 - (bit & 7))) & 1 : 0;
    v = (v << 1) | b;
  }
  return uint32_t(v);
}

// Packet header fields.
inline uint32_t PacketFrameCount(const uint8_t* p) { return p[0] >> 2; }  // XMA2 only
inline uint32_t PacketFrameOffset(const uint8_t* p) {
  return ((uint32_t(p[0] & 3) << 13) | (uint32_t(p[1]) << 5) | (p[2] >> 3)) + kBitsPerPacketHeader;
}
inline bool PacketIsXma2(const uint8_t* p) { return (p[2] & 7) == 1; }
inline uint32_t PacketSkipCount(const uint8_t* p) { return p[3]; }

struct PacketInfo {
  uint32_t frame_count = 0;
  uint32_t current_frame = 0;
  uint32_t current_frame_size = 0;
  bool LastFrameInPacket() const { return frame_count == 0 || current_frame == frame_count - 1; }
};

// Output ring over guest memory (offsets in bytes; read == write means empty).
struct OutputRing {
  uint8_t* data;
  uint32_t capacity, read, write;
  uint32_t free_bytes() const {
    if (read == write) return capacity;
    return write < read ? read - write : capacity - write + read;
  }
  bool empty_after_write() const { return read == write; }
  void Write(const uint8_t* src, uint32_t n) {
    while (n && capacity) {
      uint32_t chunk = std::min(n, capacity - write);
      std::memcpy(data + write, src, chunk);
      src += chunk;
      n -= chunk;
      write = (write + chunk) % capacity;
    }
  }
};

}  // namespace

struct DecoderState {
  std::mutex lock;
  std::atomic<bool> busy{false};
  std::atomic<bool> enabled{false};  // XMAEnableContext .. XMADisableContext
  bool allocated = false;

  const AVCodec* codec = nullptr;
  AVCodecContext* av = nullptr;
  AVPacket* packet = nullptr;
  AVFrame* frame = nullptr;

  std::array<uint8_t, kBytesPerPacketData * 2> input{};
  std::array<uint8_t, 1 + 4096 + 8> xma_frame{};
  std::array<uint8_t, kBytesPerFrameChannel * 2> raw{};      // frame ready for output
  std::array<uint8_t, kBytesPerFrameChannel * 2> decoded{};  // decoder output, before realignment
  std::array<uint8_t, kBytesPerFrameChannel * 2> carry{};    // tail of the previous decoded frame

  int32_t free_blocks = 0;
  uint32_t frame_remaining_subframes = 0;
  uint32_t loop_output_limit = 0;
  bool loop_start_skip_pending = false;
  bool carry_valid = false;
  uint32_t pending_output_limit = 0;
  uint32_t pending_start_skip = 0;

  ~DecoderState() {
    if (av) avcodec_free_context(&av);
    if (frame) av_frame_free(&frame);
    if (packet) av_packet_free(&packet);
  }

  void ResetCodecHistory() {
    // A new stream must not inherit the previous one's overlap-add tail. xmaframes has no flush
    // callback, so the codec is reopened on the next decode instead.
    if (av) avcodec_free_context(&av);
    raw.fill(0);
    decoded.fill(0);
    carry.fill(0);
    carry_valid = false;
    pending_output_limit = 0;
    pending_start_skip = 0;
    frame_remaining_subframes = 0;
    loop_output_limit = 0;
    loop_start_skip_pending = false;
  }

  bool EnsureCodec(uint32_t rate_id, bool stereo) {
    int rate = kSampleRates[std::min(rate_id, 3u)];
    int channels = stereo ? 2 : 1;
    if (av && av->sample_rate == rate && av->channels == channels) return true;
    if (av) {
      avcodec_free_context(&av);
      carry_valid = false;  // the reopened codec restarts its output
    }
    av = avcodec_alloc_context3(codec);
    if (!av) return false;
    av->sample_rate = rate;
    av->channels = channels;
    av->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;
    if (avcodec_open2(av, codec, nullptr) < 0) {
      avcodec_free_context(&av);
      return false;
    }
    return true;
  }
};

namespace {
std::atomic<uint32_t> g_kicks{0}, g_frames{0}, g_failed{0}, g_blocks{0}, g_no_output{0}, g_peak{0};
}

struct XmaEngine::Impl {
  rex::memory::Memory* memory = nullptr;
  uint32_t array_virtual = 0, array_physical = 0;
  std::array<DecoderState, kContextCount> states;
  std::mutex alloc_lock;

  // Enabled contexts are decoded continuously, as by the hardware: whenever the game frees output
  // blocks or validates an input buffer, the next sweep picks the change up.
  std::thread worker;
  std::atomic<bool> running{false};
  std::mutex wake_lock;
  std::condition_variable wake;

  void WorkerMain() {
    while (running) {
      for (int i = 0; i < int(kContextCount) && running; i++) {
        DecoderState& s = states[i];
        if (!s.enabled) continue;
        std::unique_lock lock(s.lock, std::try_to_lock);
        if (!lock || !s.allocated || !s.enabled) continue;
        s.busy = true;
        Pass(i, false);
        s.busy = false;
      }
      std::unique_lock lock(wake_lock);
      wake.wait_for(lock, std::chrono::milliseconds(2));
    }
  }

  uint8_t* Context(int index) const { return memory->TranslateVirtual(array_virtual + index * kContextSize); }
  uint8_t* Physical(uint32_t address) const { return memory->TranslatePhysical(address); }

  // --- decode pass (one "kick") ---
  void Pass(int index, bool kicked) {
    DecoderState& s = states[index];
    uint8_t* guest = Context(index);
    Ctx c;
    c.Load(guest);
    const Ctx initial = c;
    if (!c[kOutValid]) {
      if (kicked) g_no_output++;  // the sweep finds full rings all the time
      return;
    }

    OutputRing out{Physical(c.d[kOutPtr]), c[kOutBlocks] * kOutputBlockBytes, c[kOutRead] * kOutputBlockBytes,
                   c[kOutWrite] * kOutputBlockBytes};
    if (out.capacity) {
      out.read %= out.capacity;
      out.write %= out.capacity;
    }
    s.free_blocks = int32_t(out.free_bytes() / kOutputBlockBytes);

    // No input: only drain what is left of the current frame.
    if ((c[kIn0Packets] | c[kIn1Packets]) == 0) {
      if (s.frame_remaining_subframes) {
        Consume(s, c, out);
        c.set(kOutWrite, out.write / kOutputBlockBytes);
        StoreMerged(guest, c, initial);
      }
      return;
    }

    const int32_t minimum = int32_t(std::max(1u, c[kSubframeDecodeCount]) + c[kOutPadding]);
    while (s.free_blocks >= minimum) {
      const Ctx before = c;
      const uint32_t remaining = s.frame_remaining_subframes;
      const int32_t free_before = s.free_blocks;
      Decode(s, c);
      Consume(s, c, out);
      if (!c.AnyInputValid() || c[kErrorStatus] == 4) break;
      // Malformed input that neither advances the stream nor produces output must not spin.
      if (std::memcmp(&before, &c, sizeof(c)) == 0 && remaining == s.frame_remaining_subframes &&
          free_before == s.free_blocks) {
        break;
      }
    }
    c.set(kOutWrite, out.write / kOutputBlockBytes);
    if (out.empty_after_write()) c.set(kOutValid, 0);  // ring full
    StoreMerged(guest, c, initial);
  }

  // Writes back what the decoder owns, keeping fields the game changed meanwhile.
  static void StoreMerged(uint8_t* guest, const Ctx& c, const Ctx& initial) {
    Ctx fresh;
    fresh.Load(guest);
    fresh.set(kLoopCount, c[kLoopCount]);
    fresh.set(kOutWrite, c[kOutWrite]);
    if (initial[kIn0Valid] && !c[kIn0Valid]) fresh.set(kIn0Valid, 0);
    if (initial[kIn1Valid] && !c[kIn1Valid]) fresh.set(kIn1Valid, 0);
    if (initial[kOutValid] && !c[kOutValid]) fresh.set(kOutValid, 0);
    fresh.set(kInReadOffset, c[kInReadOffset]);
    fresh.set(kErrorStatus, c[kErrorStatus]);
    fresh.set(kCurrentBuffer, c[kCurrentBuffer]);
    fresh.Store(guest);
  }

  static void SwapInput(Ctx& c) {
    c.set(c.current() ? kIn1Valid : kIn0Valid, 0);
    c.set(kCurrentBuffer, c.current() ^ 1);
    c.set(kInReadOffset, kBitsPerPacketHeader);
  }

  static PacketInfo GetPacketInfo(const uint8_t* packet, uint32_t frame_offset) {
    PacketInfo info;
    const uint32_t first = PacketFrameOffset(packet);
    if (frame_offset < first) {
      info.current_frame = 0;
      info.current_frame_size = first - frame_offset;
    }
    uint32_t pos = first;
    while (kBitsPerPacket - std::min(pos, kBitsPerPacket) >= kBitsPerFrameHeader) {
      uint32_t size = PeekBits(packet, kBitsPerPacket, pos, kBitsPerFrameHeader);
      if (size == 0 || size == kMaxFrameLength) break;
      if (pos == frame_offset) {
        info.current_frame = info.frame_count;
        info.current_frame_size = size;
      }
      info.frame_count++;
      if (size > kBitsPerPacket - pos) break;
      pos += size;
      // The last bit of a frame says whether another frame follows in this packet.
      if (PeekBits(packet, kBitsPerPacket, pos - 1, 1) == 0) break;
    }
    if (PacketIsXma2(packet)) {
      uint32_t count = PacketFrameCount(packet);
      if (count > info.frame_count) {
        if (info.current_frame_size == 0) info.current_frame = info.frame_count;
        info.frame_count = count;
      }
    }
    return info;
  }

  const uint8_t* NextPacket(const Ctx& c, uint32_t next_index, uint32_t packet_count) const {
    if (next_index < packet_count) return Physical(c.InputAddress(c.current())) + next_index * kBytesPerPacket;
    uint32_t other = c.current() ^ 1;
    if (!c.InputValid(other) || !c.InputAddress(other)) return nullptr;
    return Physical(c.InputAddress(other));
  }

  static uint32_t NextPacketReadOffset(const uint8_t* buffer, uint32_t next_index, uint32_t packet_count) {
    for (; next_index < packet_count; next_index++) {
      uint32_t offset = PacketFrameOffset(buffer + next_index * kBytesPerPacket);
      if (offset <= kMaxFrameSizeInBits) return next_index * kBitsPerPacket + offset;
    }
    return kBitsPerPacketHeader;
  }

  void Decode(DecoderState& s, Ctx& c) {
    if (!c.AnyInputValid() || s.frame_remaining_subframes > 0) return;
    if (!c.InputValid(c.current())) {
      SwapInput(c);
      if (!c.InputValid(c.current())) return;
    }
    if (!c[kOutBlocks]) return;

    const uint8_t* buffer = Physical(c.InputAddress(c.current()));
    s.input.fill(0);

    bool loop_end_frame = false;
    if (c[kLoopCount] > 0) loop_end_frame = c[kInReadOffset] == std::max(kBitsPerPacketHeader, c[kLoopEnd]);

    if (c[kInReadOffset] < kBitsPerPacketHeader) c.set(kInReadOffset, kBitsPerPacketHeader);
    const uint32_t packet_count = c.InputPackets(c.current());
    const uint32_t read_offset = c[kInReadOffset];
    if (read_offset >= packet_count * kBitsPerPacket) {
      REXLOG_WARN("[native-xma] read offset {} past the input buffer ({} packets)", read_offset, packet_count);
      SwapInput(c);
      return;
    }
    const uint32_t packet_index = read_offset / kBitsPerPacket;
    const uint8_t* packet = buffer + packet_index * kBytesPerPacket;
    const uint32_t first_frame = PacketFrameOffset(packet);
    uint32_t relative = read_offset % kBitsPerPacket;
    if (relative < first_frame) {
      c.set(kInReadOffset, packet_index * kBitsPerPacket + first_frame);
      relative = first_frame;
    }

    const uint32_t skip = PacketSkipCount(packet);
    if (skip == 0xFF) {  // no frame starts in this packet
      uint32_t next = NextPacketReadOffset(buffer, packet_index + 1, packet_count);
      if (next == kBitsPerPacketHeader) SwapInput(c);
      c.set(kInReadOffset, next);
      return;
    }

    PacketInfo info = GetPacketInfo(packet, relative);
    const uint32_t next_packet_index = packet_index + skip + 1;

    // The frame's length field itself straddles the packet boundary.
    if (info.current_frame_size == 0) {
      const uint8_t* next = NextPacket(c, next_packet_index, packet_count);
      if (!next) {
        SwapInput(c);
        return;
      }
      std::memcpy(s.input.data(), packet + 4, kBytesPerPacketData);
      std::memcpy(s.input.data() + kBytesPerPacketData, next + 4, kBytesPerPacketData);
      uint32_t size = PeekBits(s.input.data(), s.input.size() * 8, relative - kBitsPerPacketHeader, kBitsPerFrameHeader);
      if (size == kMaxFrameLength) {
        c.set(kErrorStatus, 4);
        return;
      }
      info.current_frame_size = size;
    }

    const uint32_t bits_left_in_packet = (packet_index + 1) * kBitsPerPacket - c[kInReadOffset];
    const uint32_t bits_in_packet = std::min(bits_left_in_packet, info.current_frame_size);
    if (bits_in_packet == 0) {
      SwapInput(c);
      return;
    }
    if (info.LastFrameInPacket() && bits_left_in_packet < info.current_frame_size) {
      const uint8_t* next = NextPacket(c, next_packet_index, packet_count);
      if (!next) {
        c.set(kErrorStatus, 4);
        return;
      }
      std::memcpy(s.input.data() + kBytesPerPacketData, next + 4, kBytesPerPacketData);
    }
    std::memcpy(s.input.data(), packet + 4, kBytesPerPacketData);

    // Copy the frame's bits, keeping their alignment within the first byte.
    const uint32_t src_bit = relative - kBitsPerPacketHeader;
    const uint32_t padding_start = src_bit & 7;
    const uint32_t frame_bits = info.current_frame_size;
    const uint32_t total_bits = padding_start + frame_bits;
    const uint32_t frame_bytes = (total_bits + 7) / 8;
    s.xma_frame.fill(0);
    const uint32_t src_byte = src_bit >> 3;
    const uint32_t available = uint32_t(s.input.size()) - std::min<uint32_t>(src_byte, uint32_t(s.input.size()));
    std::memcpy(s.xma_frame.data() + 1, s.input.data() + src_byte, std::min(frame_bytes, available));
    s.xma_frame[1] &= uint8_t(0xFF >> padding_start);
    if (total_bits & 7) s.xma_frame[frame_bytes] &= uint8_t(0xFF << (8 - (total_bits & 7)));

    s.decoded.fill(0);
    const bool stereo = c[kStereo] != 0;
    bool decoded = false;
    if (s.EnsureCodec(c[kSampleRate], stereo)) {
      s.packet->data = s.xma_frame.data();
      s.packet->size = int(1 + frame_bytes);
      const uint32_t padding_end = frame_bytes * 8 - total_bits;
      s.xma_frame[0] = uint8_t(((padding_start & 7) << 5) | ((padding_end & 7) << 2));
      if (avcodec_send_packet(s.av, s.packet) >= 0 && avcodec_receive_frame(s.av, s.frame) >= 0) {
        ConvertFrame(s.frame, stereo, s.decoded.data());
        decoded = true;
        g_frames++;
      }
    }

    if (decoded) {
      // Realign onto the bitstream's sample numbering: this frame's samples start
      // kDecoderStartPadding into the decoded block and end in the head of the next one.
      const uint32_t pad_bytes = (kDecoderStartPadding * 2) << stereo;
      const uint32_t carry_bytes = (kBytesPerFrameChannel << stereo) - pad_bytes;
      const uint32_t output_limit = loop_end_frame ? (c[kLoopSubframeEnd] + 1) << stereo : 0;
      const uint32_t start_skip = s.loop_start_skip_pending ? c[kLoopSubframeSkip] << stereo : 0;
      s.loop_start_skip_pending = false;
      if (s.carry_valid) {
        std::memcpy(s.raw.data(), s.carry.data(), carry_bytes);
        std::memcpy(s.raw.data() + carry_bytes, s.decoded.data(), pad_bytes);
        s.frame_remaining_subframes = kBlocksPerFrameChannel << stereo;
        s.loop_output_limit = s.pending_output_limit;
        s.frame_remaining_subframes -= std::min(s.pending_start_skip, s.frame_remaining_subframes);
      }
      std::memcpy(s.carry.data(), s.decoded.data() + pad_bytes, carry_bytes);
      s.carry_valid = true;
      s.pending_output_limit = output_limit;
      s.pending_start_skip = start_skip;
    } else {
      g_failed++;
      s.carry_valid = false;  // a dropped frame breaks the adjacency of the carried tail
    }

    // Where the next frame starts.
    if (loop_end_frame) {
      c.set(kInReadOffset, std::max(kBitsPerPacketHeader, c[kLoopStart]));
      s.loop_start_skip_pending = true;
      if (c[kLoopCount] < 255) c.set(kLoopCount, c[kLoopCount] - 1);
      return;
    }
    if (!info.LastFrameInPacket()) {
      uint32_t next_frame = (c[kInReadOffset] + bits_in_packet) % kBitsPerPacket;
      c.set(kInReadOffset, packet_index * kBitsPerPacket + next_frame);
      return;
    }
    uint32_t next = NextPacketReadOffset(buffer, next_packet_index, packet_count);
    if (next == kBitsPerPacketHeader) {
      SwapInput(c);
      if (c.AnyInputValid()) {
        next = PacketFrameOffset(Physical(c.InputAddress(c.current())));
        if (next > kMaxFrameSizeInBits) {
          SwapInput(c);
          return;
        }
      }
    }
    c.set(kInReadOffset, next);
  }

  static void Consume(DecoderState& s, const Ctx& c, OutputRing& out) {
    if (!s.frame_remaining_subframes) return;
    const bool stereo = c[kStereo] != 0;
    const uint32_t total = kBlocksPerFrameChannel << stereo;
    const uint32_t consumed = total - s.frame_remaining_subframes;
    if (s.loop_output_limit > 0 && consumed >= s.loop_output_limit) {
      s.free_blocks -= int32_t(c[kOutPadding]);
      s.frame_remaining_subframes = 0;
      s.loop_output_limit = 0;
      return;
    }
    uint32_t to_write = std::min(s.frame_remaining_subframes, std::max(1u, c[kSubframeDecodeCount]));
    if (s.loop_output_limit > 0) to_write = std::min(to_write, s.loop_output_limit - consumed);
    out.Write(s.raw.data() + kOutputBlockBytes * consumed, to_write * kOutputBlockBytes);
    g_blocks += to_write;
    const uint32_t headroom = s.frame_remaining_subframes == to_write ? c[kOutPadding] : 0;
    s.free_blocks -= int32_t(to_write + headroom);
    s.frame_remaining_subframes -= to_write;
  }

  // Planar float -> interleaved big-endian 16-bit PCM (saturated: FFmpeg can exceed [-1, 1]).
  static void ConvertFrame(const AVFrame* frame, bool stereo, uint8_t* out) {
    auto* o = reinterpret_cast<uint16_t*>(out);
    const auto* l = reinterpret_cast<const float*>(frame->data[0]);
    const auto* r = stereo ? reinterpret_cast<const float*>(frame->data[1]) : nullptr;
    uint32_t n = std::min<uint32_t>(kSamplesPerFrame, uint32_t(std::max(frame->nb_samples, 0)));
    auto pcm = [](float v) {
      v = std::clamp(v, -1.0f, 1.0f) * 32767.0f;
      return native_host::ByteSwap16(uint16_t(int16_t(std::lrintf(v))));
    };
    float peak = 0.0f;
    for (uint32_t i = 0; i < n; i++) peak = std::max({peak, std::fabs(l[i]), r ? std::fabs(r[i]) : 0.0f});
    uint32_t p = uint32_t(std::min(peak, 1.0f) * 32767.0f), prev = g_peak.load();
    while (p > prev && !g_peak.compare_exchange_weak(prev, p)) {
    }
    for (uint32_t i = 0; i < n; i++) {
      if (stereo) {
        o[i * 2] = pcm(l[i]);
        o[i * 2 + 1] = pcm(r[i]);
      } else {
        o[i] = pcm(l[i]);
      }
    }
  }
};

XmaEngine& XmaEngine::Get() {
  static XmaEngine engine;
  return engine;
}

XmaEngine::XmaEngine() : impl_(std::make_unique<Impl>()) {}
XmaEngine::~XmaEngine() = default;

bool XmaEngine::Initialize(rex::memory::Memory* memory) {
  if (impl_->memory) return true;
  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!codec) {
    REXLOG_ERROR("[native-xma] FFmpeg has no xmaframes decoder");
    return false;
  }
  // The decoder reads contexts by physical address and the XDK computes indices from it, so the
  // array lives in physical memory like the hardware's.
  uint32_t array = memory->SystemHeapAlloc(kContextCount * kContextSize, 256, rex::memory::kSystemHeapPhysical);
  if (!array) {
    REXLOG_ERROR("[native-xma] unable to allocate the context array");
    return false;
  }
  std::memset(memory->TranslateVirtual(array), 0, kContextCount * kContextSize);
  impl_->memory = memory;
  impl_->array_virtual = array;
  impl_->array_physical = memory->GetPhysicalAddress(array);
  for (auto& s : impl_->states) {
    s.codec = codec;
    s.packet = av_packet_alloc();
    s.frame = av_frame_alloc();
    if (!s.packet || !s.frame) return false;
  }
  impl_->running = true;
  impl_->worker = std::thread([impl = impl_.get()] { impl->WorkerMain(); });
  REXLOG_INFO("[native-xma] {} decoder contexts at {:08X} (physical {:08X})", kContextCount, array,
              impl_->array_physical);
  return true;
}

void XmaEngine::Shutdown() {
  if (!impl_->memory) return;
  if (impl_->worker.joinable()) {
    impl_->running = false;
    impl_->wake.notify_all();
    impl_->worker.join();
  }
  for (auto& s : impl_->states) {
    std::lock_guard lock(s.lock);
    s.allocated = false;
    s.ResetCodecHistory();
  }
}

bool XmaEngine::ready() const { return impl_->memory != nullptr; }
uint32_t XmaEngine::array_physical() const { return impl_->array_physical; }

uint32_t XmaEngine::Allocate() {
  if (!ready()) return 0;
  std::lock_guard alloc(impl_->alloc_lock);
  for (int i = 0; i < int(kContextCount); i++) {
    DecoderState& s = impl_->states[i];
    std::lock_guard lock(s.lock);
    if (s.allocated) continue;
    s.allocated = true;
    s.ResetCodecHistory();
    std::memset(impl_->Context(i), 0, kContextSize);
    return impl_->array_virtual + i * kContextSize;
  }
  REXLOG_ERROR("[native-xma] out of decoder contexts");
  return 0;
}

void XmaEngine::Release(uint32_t context) {
  int i = IndexOf(context);
  if (i < 0) return;
  std::lock_guard alloc(impl_->alloc_lock);
  DecoderState& s = impl_->states[i];
  s.enabled = false;
  std::lock_guard lock(s.lock);
  s.allocated = false;
  s.ResetCodecHistory();
  std::memset(impl_->Context(i), 0, kContextSize);
}

int XmaEngine::IndexOf(uint32_t address) const {
  if (!ready()) return -1;
  uint32_t a = address;
  if (a >= impl_->array_virtual && a < impl_->array_virtual + kContextCount * kContextSize) {
    a -= impl_->array_virtual;
  } else {
    a = impl_->memory->GetPhysicalAddress(address);
    if (a < impl_->array_physical || a >= impl_->array_physical + kContextCount * kContextSize) return -1;
    a -= impl_->array_physical;
  }
  return int(a / kContextSize);
}

uint32_t XmaEngine::ContextAddress(int index) const { return impl_->array_virtual + index * kContextSize; }
uint8_t* XmaEngine::ContextMemory(int index) const { return impl_->Context(index); }

void XmaEngine::Kick(int index) {
  if (index < 0 || index >= int(kContextCount)) return;
  DecoderState& s = impl_->states[index];
  std::lock_guard lock(s.lock);
  if (!s.allocated) return;
  s.enabled = true;
  s.busy = true;
  g_kicks++;
  impl_->Pass(index, true);
  s.busy = false;
}

bool XmaEngine::Disable(int index, bool wait) {
  if (index < 0 || index >= int(kContextCount)) return true;
  impl_->states[index].enabled = false;
  return Lock(index, wait);
}

bool XmaEngine::Lock(int index, bool wait) {
  if (index < 0 || index >= int(kContextCount)) return true;
  DecoderState& s = impl_->states[index];
  if (!wait) {
    if (!s.lock.try_lock()) return false;
    s.lock.unlock();
    return true;
  }
  std::lock_guard lock(s.lock);
  return true;
}

void XmaEngine::Clear(int index) {
  if (index < 0 || index >= int(kContextCount)) return;
  DecoderState& s = impl_->states[index];
  std::lock_guard lock(s.lock);
  uint8_t* guest = impl_->Context(index);
  Ctx c;
  c.Load(guest);
  c.set(kIn0Valid, 0);
  c.set(kIn1Valid, 0);
  c.set(kOutValid, 0);
  c.set(kInReadOffset, kBitsPerPacketHeader);
  c.set(kOutRead, 0);
  c.set(kOutWrite, 0);
  c.Store(guest);
  s.ResetCodecHistory();
}

XmaEngine::Stats XmaEngine::TakeStats() {
  Stats st;
  st.kicks = g_kicks.exchange(0);
  st.frames = g_frames.exchange(0);
  st.failed = g_failed.exchange(0);
  st.blocks = g_blocks.exchange(0);
  st.no_output = g_no_output.exchange(0);
  st.peak = g_peak.exchange(0);
  return st;
}

bool XmaEngine::Busy(int index) const {
  if (index < 0 || index >= int(kContextCount)) return false;
  return impl_->states[index].busy.load();
}

XmaEngine::Guard XmaEngine::LockContext(int index) {
  impl_->states[index].lock.lock();
  return Guard{impl_.get(), index};
}

XmaEngine::Guard::~Guard() {
  if (impl) impl->states[index].lock.unlock();
}

}  // namespace bf2audio
