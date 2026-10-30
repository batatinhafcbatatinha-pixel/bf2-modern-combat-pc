#include "../platform/native_byteorder.h"
#include "audio_system.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <thread>

#include <SDL3/SDL.h>

#include <rex/logging.h>
#include "pc_settings.h"
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include "xma_engine.h"

namespace bf2audio {

namespace {

// The output device stopped consuming (NativeAudioSystem::WorkerMain): drop the output.
std::atomic<bool> g_output_stalled{false};

NativeAudioSystem* g_system = nullptr;

// Frames kept queued on the host stream before the guest is asked for more (~32 ms).
constexpr uint32_t kTargetQueuedFrames = 6;

inline float BeFloat(const uint8_t* p) {
  uint32_t v = native_host::ByteSwap32(*reinterpret_cast<const uint32_t*>(p));
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

}  // namespace

NativeAudioSystem* AudioSystem() { return g_system; }

NativeAudioSystem::NativeAudioSystem(rex::runtime::FunctionDispatcher* dispatcher) : dispatcher_(dispatcher) {
  g_system = this;
}

NativeAudioSystem::~NativeAudioSystem() {
  Shutdown();
  if (g_system == this) g_system = nullptr;
}

X_STATUS NativeAudioSystem::Setup(rex::system::KernelState* kernel_state) {
  kernel_state_ = kernel_state;
  if (!XmaEngine::Get().Initialize(dispatcher_->memory())) return X_STATUS_UNSUCCESSFUL;
  if (!OpenDevice()) REXLOG_WARN("[native-audio] no audio device; frames will be discarded");
  running_ = true;
  worker_ = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
        WorkerMain();
        return 0;
      }));
  worker_->set_name("Native Audio");
  worker_->Create();
  REXLOG_INFO("[native-audio] native audio ready ({} output channels, no APU emulation)", device_channels_);
  return X_STATUS_SUCCESS;
}

bool NativeAudioSystem::OpenDevice() {
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXLOG_ERROR("[native-audio] SDL audio init failed: {}", SDL_GetError());
    return false;
  }
  sdl_audio_ = true;
  device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
  if (!device_) {
    REXLOG_ERROR("[native-audio] unable to open the playback device: {}", SDL_GetError());
    return false;
  }
  SDL_AudioSpec spec{};
  int frames = 0;
  if (SDL_GetAudioDeviceFormat(device_, &spec, &frames)) device_channels_ = spec.channels >= 6 ? 6 : 2;
  const char* name = SDL_GetAudioDeviceName(device_);
  REXLOG_INFO("[native-audio] device '{}': {} ch {} Hz; mixing to {} ch", name ? name : "?", spec.channels,
              spec.freq, device_channels_);
  return true;
}

uint32_t NativeAudioSystem::QueuedFrames(const Client& c) const {
  if (!c.stream) return 0;
  int bytes = SDL_GetAudioStreamQueued(c.stream);
  return bytes <= 0 ? 0 : uint32_t(bytes) / (kFrameSamples * device_channels_ * sizeof(float));
}

X_STATUS NativeAudioSystem::RegisterClient(uint32_t callback, uint32_t callback_arg, uint32_t* out_index) {
  std::lock_guard lock(lock_);
  uint32_t index = kMaxClients;
  for (uint32_t i = 0; i < kMaxClients; i++) {
    if (!clients_[i].in_use) {
      index = i;
      break;
    }
  }
  if (index == kMaxClients) return X_STATUS_NO_MEMORY;
  auto* memory = dispatcher_->memory();
  Client& c = clients_[index];
  // The driver passes the callback a pointer to the argument, not the argument itself.
  c.callback_arg_ptr = memory->SystemHeapAlloc(4);
  rex::memory::store_and_swap<uint32_t>(memory->TranslateVirtual(c.callback_arg_ptr), callback_arg);
  c.callback = callback;
  c.stream = nullptr;
  if (device_) {
    SDL_AudioSpec src{SDL_AUDIO_F32LE, int(device_channels_), int(kSampleRate)};
    c.stream = SDL_CreateAudioStream(&src, nullptr);
    if (c.stream && !SDL_BindAudioStream(device_, c.stream)) {
      REXLOG_ERROR("[native-audio] unable to bind the client stream: {}", SDL_GetError());
      SDL_DestroyAudioStream(c.stream);
      c.stream = nullptr;
    }
    if (c.stream) {
      SDL_SetAudioStreamGetCallback(
          c.stream,
          [](void* self, SDL_AudioStream*, int, int) { static_cast<NativeAudioSystem*>(self)->wake_.notify_one(); },
          this);
    }
  }
  c.in_use = true;
  *out_index = index;
  REXLOG_INFO("[native-audio] render client {} callback {:08X} arg {:08X}", index, callback, callback_arg);
  wake_.notify_one();
  return X_STATUS_SUCCESS;
}

void NativeAudioSystem::UnregisterClient(uint32_t index) {
  if (index >= kMaxClients) return;
  std::lock_guard lock(lock_);
  Client& c = clients_[index];
  if (!c.in_use) return;
  if (c.stream) SDL_DestroyAudioStream(c.stream);
  if (c.callback_arg_ptr) dispatcher_->memory()->SystemHeapFree(c.callback_arg_ptr);
  c = Client{};
}

void NativeAudioSystem::SubmitFrame(uint32_t index, uint32_t samples) {
  if (index >= kMaxClients) return;
  SDL_AudioStream* stream;
  {
    std::lock_guard lock(lock_);
    if (!clients_[index].in_use) return;
    stream = clients_[index].stream;
  }
  if (!stream || !samples) return;
  // Guest frame: 6 channels of 256 big-endian floats each (FL FR FC LFE SL SR), one after another.
  const uint8_t* in = dispatcher_->memory()->TranslateVirtual(samples);
  auto ch = [&](uint32_t c, uint32_t i) { return BeFloat(in + (c * kFrameSamples + i) * 4); };
  float out[kFrameSamples * kFrameChannels];
  if (device_channels_ == 6) {
    for (uint32_t i = 0; i < kFrameSamples; i++)
      for (uint32_t c = 0; c < 6; c++) out[i * 6 + c] = std::clamp(ch(c, i), -1.0f, 1.0f);
  } else {
    // Stereo fold: center and surrounds at -3 dB, normalized so a full-scale front+center mix
    // doesn't clip; LFE is dropped (as on a stereo TV output).
    constexpr float kSide = 0.70710678f, kScale = 0.58578644f;
    for (uint32_t i = 0; i < kFrameSamples; i++) {
      float mid = ch(2, i) * kSide;
      out[i * 2] = std::clamp((ch(0, i) + mid + ch(4, i) * kSide) * kScale, -1.0f, 1.0f);
      out[i * 2 + 1] = std::clamp((ch(1, i) + mid + ch(5, i) * kSide) * kScale, -1.0f, 1.0f);
    }
  }
  // PC settings volume (0 while the settings menu holds the game).
  SDL_SetAudioStreamGain(stream, pcs::Volume());
  if (!g_output_stalled.load(std::memory_order_relaxed))
    SDL_PutAudioStreamData(stream, out, int(kFrameSamples * device_channels_ * sizeof(float)));
  submitted_++;
  float peak = 0.0f;
  for (uint32_t i = 0; i < kFrameSamples * device_channels_; i++) peak = std::max(peak, std::fabs(out[i]));
  uint32_t p = uint32_t(peak * 32767.0f), prev = peak_.load();
  while (p > prev && !peak_.compare_exchange_weak(prev, p)) {
  }
}

void NativeAudioSystem::WorkerMain() {
  auto* thread = rex::system::XThread::GetCurrentThread();
  // BF2_AUDIO_LOG=1: per-second render driver and XMA counters.
  const char* log_env = std::getenv("BF2_AUDIO_LOG");
  bool log = log_env && *log_env && *log_env != '0';
  auto log_start = std::chrono::steady_clock::now();
  uint32_t callbacks = 0;
  // An output device that stops consuming (a TV or receiver switched off) keeps the stream full,
  // and the guest would wait on its mixer callbacks forever: after 200 ms without progress the
  // worker paces by the clock, as without a device, and drops the output until it plays again.
  // BF2_AUDIO_STALL_TEST=<s> pauses the device after s seconds to exercise this.
  auto last_progress = std::chrono::steady_clock::now();
  uint32_t last_queued = 0;
  const char* stall_env = std::getenv("BF2_AUDIO_STALL_TEST");
  const double stall_test = stall_env ? std::atof(stall_env) : 0.0;
  const auto worker_start = last_progress;
  bool stall_test_done = false;
  auto clock_next = last_progress;
  while (running_) {
    if (log && std::chrono::steady_clock::now() - log_start >= std::chrono::seconds(1)) {
      auto x = XmaEngine::Get().TakeStats();
      REXLOG_INFO("[native-audio] callbacks {} submitted {} peak {} queued {} | xma kicks {} frames {} failed {} "
                  "blocks {} no-output {} peak {}",
                  callbacks, submitted_.exchange(0), peak_.exchange(0),
                  clients_[0].stream ? QueuedFrames(clients_[0]) : 0, x.kicks,
                  x.frames, x.failed, x.blocks, x.no_output, x.peak);
      callbacks = 0;
      log_start = std::chrono::steady_clock::now();
    }
    bool stalled = false;
    {
      std::lock_guard guard(lock_);
      const auto now = std::chrono::steady_clock::now();
      SDL_AudioStream* main_stream = clients_[0].in_use ? clients_[0].stream : nullptr;
      if (main_stream && stall_test > 0 && !stall_test_done &&
          std::chrono::duration<double>(now - worker_start).count() >= stall_test) {
        stall_test_done = true;
        SDL_PauseAudioStreamDevice(main_stream);
      }
      const uint32_t queued = main_stream ? QueuedFrames(clients_[0]) : 0;
      if (!main_stream || queued < last_queued || queued < kTargetQueuedFrames) last_progress = now;
      last_queued = queued;
      stalled = main_stream && now - last_progress > std::chrono::milliseconds(200);
      if (stalled != g_output_stalled.load(std::memory_order_relaxed)) {
        g_output_stalled = stalled;
        REXLOG_WARN("[native-audio] output device {}", stalled ? "stalled, pacing by the clock" : "playing again");
      }
    }
    uint32_t due[kMaxClients][2];
    uint32_t count = 0;
    {
      std::unique_lock lock(lock_);
      for (uint32_t i = 0; i < kMaxClients; i++) {
        const Client& c = clients_[i];
        // Without a device the guest is paced by the clock instead (one frame per 5.33 ms).
        if (c.in_use && c.callback && (!c.stream || stalled || QueuedFrames(c) < kTargetQueuedFrames)) {
          due[count][0] = c.callback;
          due[count][1] = c.callback_arg_ptr;
          count++;
        }
      }
      if (!count) {
        wake_.wait_for(lock, std::chrono::milliseconds(2));
        continue;
      }
    }
    for (uint32_t i = 0; i < count && running_; i++) {
      uint64_t args[] = {due[i][1]};
      dispatcher_->Execute(thread->thread_state(), due[i][0], args, 1);
      callbacks++;
    }
    if (!device_ || stalled) {
      // One 256-sample frame per 5.333 ms, on a fixed schedule (a plain sleep after the
      // callbacks ran slower than the 48 kHz clock).
      const auto now = std::chrono::steady_clock::now();
      if (now - clock_next > std::chrono::milliseconds(50)) clock_next = now;
      clock_next += std::chrono::nanoseconds(5333333);
      std::this_thread::sleep_until(clock_next);
    }
  }
}

void NativeAudioSystem::Shutdown() {
  if (worker_) {
    running_ = false;
    wake_.notify_all();
    // The worker may be blocked inside a guest callback; don't wait forever for it.
    uint64_t timeout = uint64_t(-5000000);  // 500 ms, relative
    if (worker_->Wait(0, 0, 0, &timeout) != 0) worker_->Terminate(0);
    worker_.reset();
  }
  {
    std::lock_guard lock(lock_);
    for (auto& c : clients_) {
      if (c.stream) SDL_DestroyAudioStream(c.stream);
      c = Client{};
    }
  }
  if (device_) {
    SDL_CloseAudioDevice(device_);
    device_ = 0;
  }
  if (sdl_audio_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_audio_ = false;
  }
  XmaEngine::Get().Shutdown();
}

}  // namespace bf2audio
