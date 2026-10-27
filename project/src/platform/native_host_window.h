#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <filesystem>
#include <rex/ui/window.h>
#ifndef _WIN32
#include <SDL3/SDL.h>
#endif

namespace native_host {
inline void* WindowHandle(rex::ui::Window* window) {
  if (!window) return nullptr;
#ifdef _WIN32
  return window->GetNativeWindowHandle();
#else
  // Requires a DXVK native build with the SDL3 WSI, not SDL2.
  return window->GetSDLWindowHandle();
#endif
}
inline std::filesystem::path ExecutableDirectory() {
#ifdef _WIN32
  wchar_t path[32768]{};
  const auto size = GetModuleFileNameW(nullptr, path, 32768);
  if (size && size < 32768) return std::filesystem::path(path).parent_path();
#else
  std::error_code error;
  auto path = std::filesystem::read_symlink("/proc/self/exe", error);
  if (!error) return path.parent_path();
#endif
  return std::filesystem::current_path();
}
#ifndef _WIN32
inline BOOL ClientRect(HWND handle, RECT* rect) {
  if (!handle || !rect) return FALSE;
  int width = 0, height = 0;
  if (!SDL_GetWindowSizeInPixels(reinterpret_cast<SDL_Window*>(handle), &width, &height)) return FALSE;
  *rect = {0, 0, width, height};
  return TRUE;
}
inline BOOL ValidWindow(HWND handle) {
  return handle && SDL_GetWindowID(reinterpret_cast<SDL_Window*>(handle)) != 0;
}
#endif
}
#ifndef _WIN32
#define GetClientRect native_host::ClientRect
#define IsWindow native_host::ValidWindow
#endif
