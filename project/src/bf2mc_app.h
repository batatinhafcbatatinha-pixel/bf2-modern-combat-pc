// bf2mc - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.
//
// Graphics: no GPU emulation plugin. The runtime runs in detached mode (no SDK graphics backend)
// and the game's Direct3D calls are served by the native D3D11 renderer in src/native_gpu/.
// Audio: no APU model either. XMA is decoded at the API level (src/native_audio/: kernel XMA*
// exports + FFmpeg) and the XAudio render driver is our own (SDL3 output).

#pragma once

#include <rex/rex_app.h>

#include "native_audio/audio_system.h"
#include "native_gpu/native_gpu.h"

class Bf2mcApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Bf2mcApp>(new Bf2mcApp(ctx, "bf2mc",
        PPCImageConfig));
  }

 protected:
  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.graphics = nullptr;
    config.gpu_plugin.clear();
    config.audio_factory = [](rex::runtime::FunctionDispatcher* dispatcher)
        -> std::unique_ptr<rex::system::IAudioSystem> {
      return std::make_unique<bf2audio::NativeAudioSystem>(dispatcher);
    };
  }

  void OnPostSetup() override { bf2n::NativeGpuInstallMmio(); }

  void OnPreLaunchModule() override {
    bf2n::NativeGpuSetWindow(window() ? window()->GetNativeWindowHandle() : nullptr);
  }

  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
