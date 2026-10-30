// SPDX-License-Identifier: GPL-3.0-or-later
//
// CEF browser setup and paint watch, included by shim.c.
//
// BeamNG draws its menus with a windowless (off-screen) CEF browser: CEF hands
// each frame to the game's render handler, as pixels (OnPaint) or as a shared
// GPU texture (OnAcceleratedPaint). The shim wraps cef_browser_host_create_browser
// and _sync, logs how the browser is set up, and puts its own functions in the
// game's render handler for those two callbacks: it counts the frames, logs
// whether they hold any picture, and passes each one on unchanged.
//
// BeamNG asks for external begin frames: CEF draws a frame only when the game
// asks, and takes the next request only once the last one is acknowledged. In
// the single-process, software-composited CEF the PS5 needs, that
// acknowledgement never comes (on the console: one empty frame, then none), so
// the shim creates the browser without external begin frames and CEF draws at
// its own frame rate. The game's later requests are then kept pending by the
// compositor and do nothing. shim:keep-begin-frame keeps BeamNG's setting.

// Offsets in CEF 150's C structs (the order of the methods in cef_client.h and
// cef_render_handler.h, after the 0x28 bytes of cef_base_ref_counted_t).
enum {
  kClientGetRenderHandler = 0xa8,  // the 17th method
  kClientMinSize = 0xb0,
  kRenderOnPaint = 0x60,
  kRenderOnAcceleratedPaint = 0x68,
  kRenderMinSize = 0x70,
  kMaxClients = 8,
};

typedef void* (*get_render_handler_fn)(void* client);
typedef void (*on_paint_fn)(void* self, void* browser, int type, size_t dirty_count, const void* dirty,
                            const void* buffer, int width, int height);
typedef void (*on_accelerated_paint_fn)(void* self, void* browser, int type, size_t dirty_count, const void* dirty,
                                        const void* info);

static struct {
  void* client;
  get_render_handler_fn original;
} g_clients[kMaxClients];
// The game's own callbacks. CEF's C wrappers are made anew for each GetRenderHandler call, all
// with the same functions; a wrapper with other functions is left alone.
static on_paint_fn g_game_on_paint;
static on_accelerated_paint_fn g_game_on_accelerated_paint;

static void* Field(void* object, size_t offset) { return *(void**)((unsigned char*)object + offset); }

static BOOL SetField(void* object, size_t offset, void* value) {
  void** slot = (void**)((unsigned char*)object + offset);
  DWORD old_protect;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) return 0;
  *slot = value;
  VirtualProtect(slot, sizeof(void*), old_protect, &old_protect);
  return 1;
}

// Whether a BGRA frame holds anything: samples a 16 x 9 grid.
static void SampleFrame(const uint32_t* pixels, int width, int height, int* visible, int* opaque) {
  *visible = *opaque = 0;
  for (int y = 0; y < 9; ++y)
    for (int x = 0; x < 16; ++x) {
      uint32_t p = pixels[(size_t)(height * (2 * y + 1) / 18) * (size_t)width + (size_t)(width * (2 * x + 1) / 32)];
      if (p >> 24) ++*opaque;
      if (p & 0xffffff) ++*visible;
    }
}

// Saves a frame as libcef_ui_frame_<number>.bmp beside this DLL, to see what CEF draws (shim:save-frames:
// each save holds up the game for the write).
static void SaveFrame(long number, const void* pixels, int width, int height) {
  WCHAR path[1024];
  char name[64] = "libcef_ui_frame_000.bmp";
  name[16] = (char)('0' + number / 100 % 10);
  name[17] = (char)('0' + number / 10 % 10);
  name[18] = (char)('0' + number % 10);
  DWORD n = ModuleDirectory(path, 980);
  for (size_t i = 0; name[i]; ++i) path[n++] = (WCHAR)name[i];
  path[n] = 0;
  HANDLE file = CreateFileW(path, 0x40000000 /* GENERIC_WRITE */, FILE_SHARE_READ, NULL, 2 /* CREATE_ALWAYS */,
                            0x80, NULL);
  if (file == INVALID_HANDLE_VALUE) {
    VkLog("cef frame could not be saved: number", (uint64_t)number, 0, 0);
    return;
  }
  uint32_t bytes = (uint32_t)width * (uint32_t)height * 4;
  unsigned char header[54] = {'B', 'M'};
  *(uint32_t*)(header + 2) = 54 + bytes;
  *(uint32_t*)(header + 10) = 54;
  *(uint32_t*)(header + 14) = 40;
  *(int32_t*)(header + 18) = width;
  *(int32_t*)(header + 22) = -height;  // top-down rows, as CEF gives them
  *(uint16_t*)(header + 26) = 1;
  *(uint16_t*)(header + 28) = 32;
  *(uint32_t*)(header + 34) = bytes;
  DWORD written;
  WriteFile(file, header, sizeof(header), &written, NULL);
  WriteFile(file, pixels, bytes, &written, NULL);
  CloseHandle(file);
  VkLog("cef frame saved: number, size", (uint64_t)number, ((uint64_t)(uint32_t)width << 32) | (uint32_t)height, 0);
}

static void OnPaintHook(void* self, void* browser, int type, size_t dirty_count, const void* dirty, const void* buffer,
                        int width, int height) {
  long count = __sync_add_and_fetch(&g_paints, 1);
  int visible = 0, opaque = 0;
  if (buffer && width > 0 && height > 0) SampleFrame(buffer, width, height, &visible, &opaque);
  if (!visible && !opaque) __sync_add_and_fetch(&g_blank_paints, 1);
  if (g_save_frames && buffer && width > 0 && height > 0 && width <= 4096 && height <= 4096 &&
      (count == 20 || count == 100 || count == 300 || count == 600))
    SaveFrame(count, buffer, width, height);
  if (buffer && type == 0 /* PET_VIEW */) OverlayTakeFrame(buffer, width, height, opaque);
  if (count <= 5 || count % 600 == 0) {
    VkLog("cef paint: number, size, type", count, ((uint64_t)(uint32_t)width << 32) | (uint32_t)height, type);
    VkLog("cef paint: of 144 samples, coloured, opaque", (uint64_t)visible, (uint64_t)opaque, 0);
  }
  g_game_on_paint(self, browser, type, dirty_count, dirty, buffer, width, height);
}

static void OnAcceleratedPaintHook(void* self, void* browser, int type, size_t dirty_count, const void* dirty,
                                   const void* info) {
  long count = __sync_add_and_fetch(&g_accelerated_paints, 1);
  if (count <= 5 || count % 600 == 0) {
    const unsigned char* p = info;
    VkLog("cef accelerated paint: number, texture handle, format", count, p ? *(const uint64_t*)(p + 8) : 0,
          p ? *(const uint32_t*)(p + 16) : 0);
  }
  g_game_on_accelerated_paint(self, browser, type, dirty_count, dirty, info);
}

// Puts the shim's callbacks in a render handler wrapper. The wrapper is CEF's heap memory.
static void WrapRenderHandler(void* handler) {
  if (*(size_t*)handler < kRenderMinSize) return;
  on_paint_fn paint = (on_paint_fn)Field(handler, kRenderOnPaint);
  on_accelerated_paint_fn accelerated = (on_accelerated_paint_fn)Field(handler, kRenderOnAcceleratedPaint);
  if (paint == OnPaintHook) return;
  VkLock();
  if (!g_game_on_paint) {
    g_game_on_paint = paint;
    g_game_on_accelerated_paint = accelerated;
    VkLog("cef render handler: OnPaint, OnAcceleratedPaint", (uint64_t)(uintptr_t)paint,
          (uint64_t)(uintptr_t)accelerated, 0);
  }
  BOOL same = paint == g_game_on_paint && accelerated == g_game_on_accelerated_paint;
  VkUnlock();
  if (!same) {
    VkLog("cef render handler with other callbacks left alone", (uint64_t)(uintptr_t)paint, 0, 0);
    return;
  }
  *(void**)((unsigned char*)handler + kRenderOnPaint) = (void*)OnPaintHook;
  *(void**)((unsigned char*)handler + kRenderOnAcceleratedPaint) = (void*)OnAcceleratedPaintHook;
}

static void* GetRenderHandlerHook(void* client) {
  get_render_handler_fn original = NULL;
  for (int i = 0; i < kMaxClients; ++i)
    if (g_clients[i].client == client) original = g_clients[i].original;
  void* handler = original ? original(client) : NULL;
  if (handler) WrapRenderHandler(handler);
  return handler;
}

static void WrapClient(void* client) {
  if (!client) {
    Log("libcef-shim: cef browser has no client\n");
    return;
  }
  if (*(size_t*)client < kClientMinSize) {
    VkLog("cef client too small to wrap: size", *(size_t*)client, 0, 0);
    return;
  }
  if (Field(client, kClientGetRenderHandler) == (void*)GetRenderHandlerHook) return;
  VkLock();
  int free_slot = -1;
  for (int i = 0; i < kMaxClients && free_slot < 0; ++i)
    if (!g_clients[i].client) free_slot = i;
  if (free_slot >= 0) {
    g_clients[free_slot].client = client;
    g_clients[free_slot].original = (get_render_handler_fn)Field(client, kClientGetRenderHandler);
    if (!SetField(client, kClientGetRenderHandler, (void*)GetRenderHandlerHook)) g_clients[free_slot].client = NULL;
  }
  VkUnlock();
}

// cef_window_info_t on Windows: size, ex_style, window_name, style, bounds (x, y, width,
// height at 44), parent_window, menu, then windowless_rendering_enabled (80),
// shared_texture_enabled (84) and external_begin_frame_enabled (88).
static void LogWindowInfo(const unsigned char* info, const unsigned char* settings) {
  if (info)
    VkLog("cef browser: windowless, shared texture, external begin frame", *(const int*)(info + 80),
          *(const int*)(info + 84), *(const int*)(info + 88));
  if (settings && *(const size_t*)settings >= 12)
    VkLog("cef browser: windowless frame rate", (uint64_t)*(const int*)(settings + 8), 0, 0);
}

// A copy of the window info without external begin frames (and, with shim:no-shared-texture,
// without shared textures), or the game's own when there is nothing to change.
static const void* AdjustWindowInfo(const unsigned char* info, unsigned char* copy, size_t capacity) {
  if (!info) return info;
  size_t size = *(const size_t*)info;
  BOOL begin_frame = !g_keep_begin_frame && *(const int*)(info + 88);
  BOOL shared = g_no_shared_texture && *(const int*)(info + 84);
  if ((!begin_frame && !shared) || size < 92 || size > capacity) return info;
  for (size_t i = 0; i < size; ++i) copy[i] = info[i];
  if (begin_frame) {
    *(int*)(copy + 88) = 0;
    Log("libcef-shim: cef browser: external begin frames turned off, CEF draws at its own rate\n");
  }
  if (shared) {
    *(int*)(copy + 84) = 0;
    Log("libcef-shim: cef browser: shared texture turned off\n");
  }
  return copy;
}

// libcef_real.dll, loaded here too in case a browser comes before cef_initialize.
static HMODULE RealLibrary(void) {
  static const WCHAR name[] = {'l', 'i', 'b', 'c', 'e', 'f', '_', 'r', 'e', 'a', 'l', '.', 'd', 'l', 'l', 0};
  if (!g_real) g_real = LoadLibraryW(name);
  return g_real;
}

typedef int (*create_browser_fn)(const void*, void*, const void*, const void*, void*, void*);
typedef void* (*create_browser_sync_fn)(const void*, void*, const void*, const void*, void*, void*);

__declspec(dllexport) int cef_browser_host_create_browser(const void* window_info, void* client, const void* url,
                                                          const void* settings, void* extra_info,
                                                          void* request_context) {
  create_browser_fn real = (create_browser_fn)GetProcAddress(RealLibrary(), "cef_browser_host_create_browser");
  LogCefString("libcef-shim: cef browser (async) for ", url);
  LogWindowInfo(window_info, settings);
  WrapClient(client);
  static unsigned char copy[256];
  return real ? real(AdjustWindowInfo(window_info, copy, sizeof(copy)), client, url, settings, extra_info,
                     request_context)
              : 0;
}

__declspec(dllexport) void* cef_browser_host_create_browser_sync(const void* window_info, void* client,
                                                                 const void* url, const void* settings,
                                                                 void* extra_info, void* request_context) {
  create_browser_sync_fn real =
      (create_browser_sync_fn)GetProcAddress(RealLibrary(), "cef_browser_host_create_browser_sync");
  LogCefString("libcef-shim: cef browser (sync) for ", url);
  LogWindowInfo(window_info, settings);
  WrapClient(client);
  static unsigned char copy[256];
  return real ? real(AdjustWindowInfo(window_info, copy, sizeof(copy)), client, url, settings, extra_info,
                     request_context)
              : NULL;
}
