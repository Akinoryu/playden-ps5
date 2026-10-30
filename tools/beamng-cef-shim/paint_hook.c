// SPDX-License-Identifier: GPL-3.0-or-later
//
// CEF paint watch, included by shim.c.
//
// BeamNG draws its menus with a windowless (off-screen) CEF browser: CEF hands
// each frame to the game's render handler, as pixels (OnPaint) or as a shared
// GPU texture (OnAcceleratedPaint). The shim wraps cef_browser_host_create_browser
// and _sync, logs how the browser is set up, and puts its own functions in the
// game's render handler for those two callbacks: it counts the frames, logs
// whether they hold any picture, and passes each one on unchanged.

// Offsets in CEF 150's C structs (the order of the methods in cef_client.h and
// cef_render_handler.h, after the 0x28 bytes of cef_base_ref_counted_t).
enum {
  kClientGetRenderHandler = 0xa8,  // the 17th method
  kClientMinSize = 0xb0,
  kRenderOnPaint = 0x60,
  kRenderOnAcceleratedPaint = 0x68,
  kRenderMinSize = 0x70,
  kMaxWrapped = 8,
};

typedef void* (*get_render_handler_fn)(void* client);
typedef void (*on_paint_fn)(void* self, void* browser, int type, size_t dirty_count, const void* dirty,
                            const void* buffer, int width, int height);
typedef void (*on_accelerated_paint_fn)(void* self, void* browser, int type, size_t dirty_count, const void* dirty,
                                        const void* info);

static struct {
  void* client;
  get_render_handler_fn original;
} g_clients[kMaxWrapped];
static struct {
  void* handler;
  on_paint_fn paint;
  on_accelerated_paint_fn accelerated;
} g_handlers[kMaxWrapped];

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

static void OnPaintHook(void* self, void* browser, int type, size_t dirty_count, const void* dirty, const void* buffer,
                        int width, int height) {
  long count = __sync_add_and_fetch(&g_paints, 1);
  int visible = 0, opaque = 0;
  if (buffer && width > 0 && height > 0) SampleFrame(buffer, width, height, &visible, &opaque);
  if (!visible && !opaque) __sync_add_and_fetch(&g_blank_paints, 1);
  if (count <= 5 || count % 600 == 0) {
    VkLog("cef paint: number, size, type", count, ((uint64_t)(uint32_t)width << 32) | (uint32_t)height, type);
    VkLog("cef paint: of 144 samples, coloured, opaque", (uint64_t)visible, (uint64_t)opaque, 0);
  }
  on_paint_fn original = NULL;
  for (int i = 0; i < kMaxWrapped; ++i)
    if (g_handlers[i].handler == self) original = g_handlers[i].paint;
  if (original) original(self, browser, type, dirty_count, dirty, buffer, width, height);
}

static void OnAcceleratedPaintHook(void* self, void* browser, int type, size_t dirty_count, const void* dirty,
                                   const void* info) {
  long count = __sync_add_and_fetch(&g_accelerated_paints, 1);
  if (count <= 5 || count % 600 == 0) {
    const unsigned char* p = info;
    VkLog("cef accelerated paint: number, texture handle, format", count, p ? *(const uint64_t*)(p + 8) : 0,
          p ? *(const uint32_t*)(p + 16) : 0);
  }
  on_accelerated_paint_fn original = NULL;
  for (int i = 0; i < kMaxWrapped; ++i)
    if (g_handlers[i].handler == self) original = g_handlers[i].accelerated;
  if (original) original(self, browser, type, dirty_count, dirty, info);
}

static void* GetRenderHandlerHook(void* client) {
  get_render_handler_fn original = NULL;
  for (int i = 0; i < kMaxWrapped; ++i)
    if (g_clients[i].client == client) original = g_clients[i].original;
  void* handler = original ? original(client) : NULL;
  if (!handler) return handler;
  VkLock();
  BOOL known = 0;
  int free_slot = -1;
  for (int i = 0; i < kMaxWrapped; ++i) {
    if (g_handlers[i].handler == handler) known = 1;
    if (!g_handlers[i].handler && free_slot < 0) free_slot = i;
  }
  if (!known && free_slot >= 0 && *(size_t*)handler >= kRenderMinSize) {
    g_handlers[free_slot].handler = handler;
    g_handlers[free_slot].paint = (on_paint_fn)Field(handler, kRenderOnPaint);
    g_handlers[free_slot].accelerated = (on_accelerated_paint_fn)Field(handler, kRenderOnAcceleratedPaint);
    BOOL ok = SetField(handler, kRenderOnPaint, (void*)OnPaintHook) &&
              SetField(handler, kRenderOnAcceleratedPaint, (void*)OnAcceleratedPaintHook);
    VkLog(ok ? "cef render handler wrapped: handler, OnPaint, OnAcceleratedPaint"
             : "cef render handler could not be wrapped",
          (uint64_t)(uintptr_t)handler, (uint64_t)(uintptr_t)g_handlers[free_slot].paint,
          (uint64_t)(uintptr_t)g_handlers[free_slot].accelerated);
  }
  VkUnlock();
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
  VkLock();
  int free_slot = -1;
  for (int i = 0; i < kMaxWrapped; ++i) {
    if (g_clients[i].client == client) free_slot = -2;
    if (free_slot == -1 && !g_clients[i].client) free_slot = i;
  }
  if (free_slot >= 0) {
    g_clients[free_slot].client = client;
    g_clients[free_slot].original = (get_render_handler_fn)Field(client, kClientGetRenderHandler);
    if (!SetField(client, kClientGetRenderHandler, (void*)GetRenderHandlerHook)) g_clients[free_slot].client = NULL;
  }
  VkUnlock();
}

static void LogWindowInfo(const unsigned char* info) {
  if (!info) return;
  const int* bounds = (const int*)(info + 44);
  VkLog("cef browser: windowless, shared texture, external begin frame", *(const int*)(info + 80),
        *(const int*)(info + 84), *(const int*)(info + 88));
  VkLog("cef browser: size", (uint64_t)(uint32_t)bounds[2], (uint64_t)(uint32_t)bounds[3], 0);
}

static BOOL g_no_shared_texture;

// A copy of the window info with shared textures off, when shim:no-shared-texture asks for it.
static const void* AdjustWindowInfo(const unsigned char* info, unsigned char* copy, size_t capacity) {
  if (!info || !g_no_shared_texture) return info;
  size_t size = *(const size_t*)info;
  if (size < 92 || size > capacity) return info;
  for (size_t i = 0; i < size; ++i) copy[i] = info[i];
  *(int*)(copy + 84) = 0;
  Log("libcef-shim: cef browser: shared texture turned off\n");
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
  if (url) LogWide("libcef-shim: cef browser (async) for ", ((const cef_string_t*)url)->str,
                   ((const cef_string_t*)url)->length);
  LogWindowInfo(window_info);
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
  if (url) LogWide("libcef-shim: cef browser (sync) for ", ((const cef_string_t*)url)->str,
                   ((const cef_string_t*)url)->length);
  LogWindowInfo(window_info);
  WrapClient(client);
  static unsigned char copy[256];
  return real ? real(AdjustWindowInfo(window_info, copy, sizeof(copy)), client, url, settings, extra_info,
                     request_context)
              : NULL;
}
