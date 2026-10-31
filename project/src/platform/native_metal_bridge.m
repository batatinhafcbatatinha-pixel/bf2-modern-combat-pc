// macOS: lets DXMT (D3D11 on Metal, built as native libraries) present into this game's SDL window.
// DXMT asks Wine's Mac driver for a Metal view of each window, through a symbol named macdrv_functions.
// There is no Wine here, so this file exports that symbol and answers with SDL's Metal view.
// The window handle given to DXGI is the SDL_Window*, as on Linux. Keep the structures below equal to those in
// DXMT's src/winemetal/unix/winemetal_unix.c at the commit pinned in tools/macos/build-deps.sh.
#if __has_include(<SDL3/SDL.h>)
#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#else
#include <SDL.h>
#include <SDL_metal.h>
#endif
#include <dispatch/dispatch.h>
#include <stdlib.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

struct macdrv_win_data {
  void *hwnd;
  void *cocoa_window;
  void *cocoa_view;
  void *client_cocoa_view;
};

struct macdrv_functions_t {
  void (*macdrv_init_display_devices)(int);
  struct macdrv_win_data *(*get_win_data)(void *hwnd);
  void (*release_win_data)(struct macdrv_win_data *data);
  void *(*macdrv_get_cocoa_window)(void *hwnd, int require_on_screen);
  void *(*macdrv_create_metal_device)(void);
  void (*macdrv_release_metal_device)(void *device);
  void *(*macdrv_view_create_metal_view)(void *view, void *device);
  void *(*macdrv_view_get_metal_layer)(void *view);
  void (*macdrv_view_release_metal_view)(void *view);
  void (*on_main_thread)(dispatch_block_t block);
};

// AppKit objects belong to the main thread; the renderer may run on another one.
static void run_on_main(dispatch_block_t block) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

static struct macdrv_win_data *get_win_data(void *hwnd) {
  struct macdrv_win_data *data = calloc(1, sizeof(*data));
  if (data) {
    data->hwnd = hwnd;
    data->client_cocoa_view = hwnd;  // Only passed back to create_metal_view below.
  }
  return data;
}

static void release_win_data(struct macdrv_win_data *data) { free(data); }

static void *create_metal_view(void *window, void *device) {
  __block SDL_MetalView view = NULL;
  run_on_main(^{
    view = SDL_Metal_CreateView((SDL_Window *)window);
    if (view) {
      CAMetalLayer *layer = (__bridge CAMetalLayer *)SDL_Metal_GetLayer(view);
      layer.device = (__bridge id<MTLDevice>)device;
    }
  });
  return view;
}

static void *get_metal_layer(void *view) { return SDL_Metal_GetLayer((SDL_MetalView)view); }

static void release_metal_view(void *view) {
  run_on_main(^{
    SDL_Metal_DestroyView((SDL_MetalView)view);
  });
}

__attribute__((visibility("default"), used)) struct macdrv_functions_t macdrv_functions = {
    .get_win_data = get_win_data,
    .release_win_data = release_win_data,
    .macdrv_view_create_metal_view = create_metal_view,
    .macdrv_view_get_metal_layer = get_metal_layer,
    .macdrv_view_release_metal_view = release_metal_view,
    .on_main_thread = run_on_main,
};
