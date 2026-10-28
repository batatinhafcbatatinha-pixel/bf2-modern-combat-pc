// Native XMA decoding for Battlefield 2: Modern Combat.
//
// The game decodes XMA (the Xbox 360's WMA Pro variant) through two clients:
// - RenderWare Audio calls the kernel's XMA* functions (audio_exports.cpp);
// - the XDK's XMAPlayback library (inside the statically linked XAudio) fills context structures
//   itself and pokes the decoder's lock/kick/clear registers; those few functions are replaced at
//   the API level as well (audio_exports.cpp), so no decoder register file exists.
//
// In Battlefield 2 only the kernel's XMA* functions are used (no XMAPlayback library).
//
// A context is the 64-byte structure the game shares with the decoder (input packet buffers, an
// output ring of 256-byte blocks, loop points). It stays in guest memory, because both clients read
// and write its fields; this engine only keeps the codec state that belongs to it. A "kick" runs one
// decode pass on the calling thread: frames are decoded with FFmpeg's WMA Pro (xmaframes) decoder
// and written to the output ring as big-endian 16-bit PCM until the ring is full or the input runs
// out - the same contract the hardware offers. Between XMAEnableContext and XMADisableContext a
// background sweep keeps running passes on the context, as the hardware keeps decoding it.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace rex::memory {
class Memory;
}

namespace bf2audio {

class XmaEngine {
 public:
  static constexpr uint32_t kContextCount = 320;
  static constexpr uint32_t kContextSize = 64;

  static XmaEngine& Get();

  bool Initialize(rex::memory::Memory* memory);
  void Shutdown();
  bool ready() const;

  // Physical address of the context array (what the XDK reads to compute context indices).
  uint32_t array_physical() const;

  // Returns the guest (virtual) address of a free, zeroed context, or 0.
  uint32_t Allocate();
  void Release(uint32_t context);
  // Index of a context by guest virtual or physical address; -1 if it isn't one of ours.
  int IndexOf(uint32_t address) const;
  uint32_t ContextAddress(int index) const;

  // Decoder commands.
  void Kick(int index);                  // enable + one decode pass now
  bool Disable(int index, bool wait);    // stop the sweep; false if a pass is running and !wait
  bool Lock(int index, bool wait);       // stop decoding; false if a pass is running and !wait
  void Clear(int index);                 // reset offsets/validity and the codec history
  bool Busy(int index) const;            // a decode pass is running right now

  // Diagnostics counters (reset by the reader).
  struct Stats {
    uint32_t kicks = 0, frames = 0, failed = 0, blocks = 0, no_output = 0, peak = 0;
  };
  Stats TakeStats();

  // Stall diagnostics: a short per-context history of API calls and decode pass outcomes, and a
  // readable dump of the context with that history.
  void Trace(int index, const char* what, uint32_t value = 0);

  // The game waits for output its stream end requires while the context has no input left (see
  // xma_engine.cpp): releases the frame the decoder holds back, completed with silence. Returns the
  // blocks written.
  uint32_t ReleaseHeldFrame(int index);
  std::string Describe(int index);

  // Runs `fn` with the context's guest structure while no decode pass can touch it.
  template <typename Fn>
  void Edit(int index, Fn&& fn) {
    auto guard = LockContext(index);
    fn(ContextMemory(index));
  }

 private:
  XmaEngine();
  ~XmaEngine();
  struct Impl;
  struct Guard {
    Impl* impl;
    int index;
    ~Guard();
  };
  Guard LockContext(int index);
  uint8_t* ContextMemory(int index) const;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bf2audio
