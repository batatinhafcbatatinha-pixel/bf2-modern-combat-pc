// Battlefield 2: Modern Combat for the in-game PC settings (pc_settings.h).
#pragma once

#include "pc_settings.h"

inline pcs::GameInfo PcGameInfo() {
  // The USA disc ships one language (8/Localisation): no language choice.
  pcs::GameInfo info;
  info.title = L"BATTLEFIELD 2: MODERN COMBAT";
  info.window_title = "Battlefield 2: Modern Combat";
  info.ini_name = L"bf2mc-pc.ini";
  info.internal_resolution = true;
  return info;
}
