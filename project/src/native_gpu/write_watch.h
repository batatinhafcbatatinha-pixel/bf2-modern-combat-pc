// Guest physical memory write tracking for the renderer's caches (texture contents, vertex mirror).
//
// Hashing every texture and vertex chunk a frame uses, every frame, costs several ms of the render
// thread. Instead, ranges that have stayed unchanged for a while are write-protected through the
// SDK's physical memory invalidation callbacks (the mechanism Xenia's GPU caches use): the first
// guest write to a protected page raises a callback that stamps the page and unprotects it. A range
// is known unchanged while none of its pages carries a stamp newer than the one taken when it was
// armed, so its hash can be skipped.
//
// Stamps instead of dirty bits: several cache entries may share a page and nothing is ever cleared,
// so one consumer can't hide a write from another. Arming order is: read the stamp, protect, then
// take the reference hash - a write racing with the arm is either seen by the hash or stamped.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

namespace bf2n {

class WriteWatch {
 public:
  static constexpr uint32_t kPageShift = 12;
  static constexpr uint32_t kPageCount = 0x20000000u >> kPageShift;

  bool Start() {
    auto* memory = rex::system::kernel_state()->memory();
    if (!memory) return false;
    stamps_ = std::make_unique<std::atomic<uint32_t>[]>(kPageCount);
    handle_ = memory->RegisterPhysicalMemoryInvalidationCallback(&Callback, this);
    return handle_ != nullptr;
  }
  ~WriteWatch() {
    if (handle_) rex::system::kernel_state()->memory()->UnregisterPhysicalMemoryInvalidationCallback(handle_);
  }
  bool active() const { return handle_ != nullptr && !disabled_; }
  // Called when a verification hash finds a change the stamps missed: some writer bypasses the
  // guest views (host code writing physical memory directly). Hashing is then used for everything.
  void Disable() { disabled_ = true; }

  // Protects [address, address + size) and returns the stamp to compare against later.
  uint32_t Arm(uint32_t address, uint32_t size) {
    uint32_t stamp = clock_.load(std::memory_order_acquire);
    rex::system::kernel_state()->memory()->EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
    return stamp;
  }
  // True when no page of the range was written since the arm that returned `stamp`.
  bool Unchanged(uint32_t address, uint32_t size, uint32_t stamp) const {
    if (!size) return true;
    uint32_t first = address >> kPageShift, last = (address + size - 1) >> kPageShift;
    if (last >= kPageCount) return false;
    for (uint32_t p = first; p <= last; ++p)
      if (stamps_[p].load(std::memory_order_acquire) > stamp) return false;
    return true;
  }
  uint64_t callbacks() const { return callbacks_.load(std::memory_order_relaxed); }

 private:
  static std::pair<uint32_t, uint32_t> Callback(void* context, uint32_t start, uint32_t length,
                                                bool /*exact_range*/) {
    auto* self = static_cast<WriteWatch*>(context);
    self->callbacks_.fetch_add(1, std::memory_order_relaxed);
    start &= 0x1FFFFFFFu;
    if (!length) return {start, 0};
    uint64_t end = std::min<uint64_t>(uint64_t(start) + length, 0x20000000u);
    uint32_t stamp = self->clock_.fetch_add(1, std::memory_order_acq_rel) + 1;
    for (uint64_t p = start >> kPageShift; p <= (end - 1) >> kPageShift; ++p)
      self->stamps_[p].store(stamp, std::memory_order_release);
    // Only the stamped range may be unwatched: anything wider would let later writes go unseen.
    uint32_t first = start & ~((1u << kPageShift) - 1);
    uint32_t last = uint32_t(((end - 1) >> kPageShift) << kPageShift) + (1u << kPageShift);
    return {first, last - first};
  }

  std::unique_ptr<std::atomic<uint32_t>[]> stamps_;
  std::atomic<uint32_t> clock_{1};
  std::atomic<uint64_t> callbacks_{0};
  void* handle_ = nullptr;
  bool disabled_ = false;
};

}  // namespace bf2n
