// Battlefield 2: Modern Combat for the in-game PC settings (pc_settings.h).
#pragma once

#include "pc_settings.h"

const pcs::Theme* PcBf2Theme();

// Indices into kEffects (switched in post_effects.cpp).
enum { kEffectBloom };

inline pcs::GameInfo PcGameInfo() {
  // The USA disc ships one language (8/Localisation): no language choice.
  static const pcs::Effect kEffects[] = {
      {"bloom", L"Bloom", L"Bloom",
       L"Brilho em volta das partes claras da imagem.", L"Glow around the bright parts of the image."},
  };
  pcs::GameInfo info;
  info.title = L"BATTLEFIELD 2: MODERN COMBAT";
  info.window_title = "Battlefield 2: Modern Combat";
  info.ini_name = L"bf2mc-pc.ini";
  info.internal_resolution = true;
  info.msaa = true;
  // Requested uncapped mode; physics/audio above 60 Hz still need gameplay validation.
  info.unlocked_fps = true;
  info.effects = kEffects;
  info.effect_count = int(sizeof(kEffects) / sizeof(kEffects[0]));
  info.theme = PcBf2Theme();
  return info;
}

