// SPDX-License-Identifier: GPL-3.0-or-later
//
// BeamNG's interface drawn with fewer pixels than its view, and a count of the shared memory CEF
// maps, included by shim.c before paint_hook.c.
//
// At Fullscreen 1920x1080 the interface took more of the console's flexible memory than a title
// has. Chromium keeps the interface's tiles and decoded images in shared memory, which on the PS5
// is flexible memory (about 440 MiB a title): the main menu alone took about 215 MiB of it, black
// squares showed where 256 KiB tiles could not be mapped, and then Chromium ended the game (out of
// memory, 0xe0000008) in menus, Options and loading screens. At 1280x720 about 215 MiB stayed free.
//
// So when BeamNG's view is wider than 1280, the shim lowers the device scale factor BeamNG reports
// to CEF (GetScreenInfo, which CEF asks again at each resize) by 1280 / the view's width. Chromium
// lays the page out at BeamNG's size as before and keeps mouse positions in it, but draws it 1280
// pixels wide. Before BeamNG gets each frame, the shim scales the parts that changed back up to the
// view's size (OnPaint), and a popup's (a <select> list) to the size OnPopupSize gave it, so BeamNG
// sees the frames it asked for. The 3D picture keeps its own size.
// shim:ui-width=N draws N pixels wide instead (640 or more), shim:ui-full at the view's size.
//
// Every 10 seconds a line gives the shared memory CEF has mapped (MapViewOfFile from
// libcef_real.dll): views, MiB, the most so far; a view that could not be mapped is logged.

typedef int (*get_screen_info_fn)(void* self, void* browser, unsigned char* info);
typedef void (*get_view_rect_fn)(void* self, void* browser, int* rect);  // cef_rect_t: x, y, width, height
typedef void (*on_popup_size_fn)(void* self, void* browser, const int* rect);
static size_t SampleAppend(char* line, size_t n, size_t capacity, const char* text);  // sampler.c
static size_t SampleNumber(char* line, size_t n, size_t capacity, uint64_t value);
static get_screen_info_fn g_game_get_screen_info;
static on_popup_size_fn g_game_on_popup_size;
static volatile float g_ui_factor = 1.0f;  // the scale factor last given to CEF, against BeamNG's
static volatile float g_ui_game_scale = 1.0f;  // BeamNG's own
static int g_ui_popup[2];  // the popup's (a <select> list's) size in the view, from OnPopupSize
static long g_ui_logged;

// BeamNG's view rect, from the render handler's get_view_rect (0x38). CEF adds a reference to the
// browser for each callback, which the game releases, so the shim adds one for this extra call.
static void UiViewRect(void* self, void* browser, int* view) {
  get_view_rect_fn view_rect = *(get_view_rect_fn*)((unsigned char*)self + 0x38);
  void (*add_ref)(void*) = browser ? *(void (**)(void*))((unsigned char*)browser + 8) : NULL;
  if (!view_rect || !add_ref) return;
  add_ref(browser);
  view_rect(self, browser, view);
}

// cef_screen_info_t: size_t size, float device_scale_factor (8), int depth, depth_per_component,
// is_monochrome, cef_rect_t rect (24), available_rect (40). CEF fills it with its defaults (scale
// 1) before the call and uses the scale even when the game returns 0.
static int GetScreenInfoHook(void* self, void* browser, unsigned char* info) {
  int view[4] = {0, 0, 0, 0};
  UiViewRect(self, browser, view);  // before the game's call, which releases the browser
  int filled = g_game_get_screen_info ? g_game_get_screen_info(self, browser, info) : 0;
  float* scale = (float*)(info + 8);
  if (*scale <= 0.0f) *scale = 1.0f;
  float pixels = (float)view[2] * *scale, factor = 1.0f;
  // A quarter pixel short of the width wanted: CEF rounds the size in pixels up.
  if (g_ui_width_option && pixels > (float)g_ui_width_option) factor = ((float)g_ui_width_option - 0.25f) / pixels;
  g_ui_game_scale = *scale;
  *scale *= factor;
  g_ui_factor = factor;
  if (g_ui_logged < 8 && factor < 1.0f) {
    ++g_ui_logged;
    VkLog("ui: view, drawn at scale (thousandths)", ((uint64_t)(uint32_t)view[2] << 32) | (uint32_t)view[3],
          (uint64_t)(*scale * 1000.0f), (uint64_t)filled);
  }
  return filled;
}

static void OnPopupSizeHook(void* self, void* browser, const int* rect) {
  if (rect) g_ui_popup[0] = rect[2], g_ui_popup[1] = rect[3];
  g_game_on_popup_size(self, browser, rect);
}

// The frame at the view's size: kept between paints, as only the parts that change are drawn.
static uint32_t* g_ui_frame[2];  // PET_VIEW, PET_POPUP
static uint32_t g_ui_frame_bytes[2];
static int g_ui_sizes[2][4];  // the last frame's size and the size it was scaled to
static int32_t g_ui_rects[2][64 * 4];

static uint32_t UiAverage(uint32_t a, uint32_t b) { return (a & b) + (((a ^ b) & 0xfefefefeu) >> 1); }

static uint32_t UiLerp(uint32_t a, uint32_t b, uint32_t w) {  // w of 256 towards b
  uint32_t rb = (((a & 0x00ff00ffu) * (256 - w) + (b & 0x00ff00ffu) * w) >> 8) & 0x00ff00ffu;
  uint32_t ag = (((a >> 8) & 0x00ff00ffu) * (256 - w) + ((b >> 8) & 0x00ff00ffu) * w) & 0xff00ff00u;
  return rb | ag;
}

// Scales [x0, x1) x [y0, y1) of to (to_width x to_height) from all of from (width x height). Each
// pixel depends only on the source, so scaling a part gives what scaling all of it would there.
// Two source pixels to three (BeamNG's 1280 -> 1920 and 720 -> 1080) is done in blocks of 3 x 3:
// rows 0 and 2 from source rows 0 and 1, the row between them their average. Anything else is
// bilinear.
static void UiScaleRegion(const uint32_t* from, int width, int height, uint32_t* to, int to_width, int to_height,
                          int x0, int y0, int x1, int y1) {
  if (width * 3 == to_width * 2 && height * 3 == to_height * 2) {  // both sizes then multiples of 3
    x0 -= x0 % 3, y0 -= y0 % 3;
    x1 += (3 - x1 % 3) % 3;
    for (int y = y0; y < y1; y += 3) {
      const uint32_t* a = from + (size_t)(y / 3 * 2) * (size_t)width;
      const uint32_t* b = a + width;
      uint32_t* top = to + (size_t)y * (size_t)to_width;
      uint32_t* middle = top + to_width;
      uint32_t* bottom = middle + to_width;
      for (int x = x0, s = x0 / 3 * 2; x < x1; x += 3, s += 2) {
        top[x] = a[s], top[x + 1] = UiAverage(a[s], a[s + 1]), top[x + 2] = a[s + 1];
        bottom[x] = b[s], bottom[x + 1] = UiAverage(b[s], b[s + 1]), bottom[x + 2] = b[s + 1];
      }
      for (int x = x0; x < x1; ++x) middle[x] = UiAverage(top[x], bottom[x]);
    }
    return;
  }
  static int columns[7680];  // per column: the source column, << 8, and the weight of the next one
  for (int x = x0; x < x1; ++x) {
    int fx = (int)(((int64_t)(2 * x + 1) * width * 128) / to_width) - 128;  // 1/256 source pixel
    columns[x] = fx < 0 ? 0 : fx;
  }
  for (int y = y0; y < y1; ++y) {
    int fy = (int)(((int64_t)(2 * y + 1) * height * 128) / to_height) - 128;
    if (fy < 0) fy = 0;
    int sy = fy >> 8, wy = fy & 255;
    const uint32_t* a = from + (size_t)(sy < height ? sy : height - 1) * (size_t)width;
    const uint32_t* b = from + (size_t)(sy + 1 < height ? sy + 1 : height - 1) * (size_t)width;
    uint32_t* row = to + (size_t)y * (size_t)to_width;
    for (int x = x0; x < x1; ++x) {
      int sx = columns[x] >> 8, wx = columns[x] & 255;
      int sx1 = sx + 1 < width ? sx + 1 : width - 1;
      row[x] = UiLerp(UiLerp(a[sx], a[sx1], (uint32_t)wx), UiLerp(b[sx], b[sx1], (uint32_t)wx), (uint32_t)wy);
    }
  }
}

// From OnPaint: when CEF drew the frame smaller than the size BeamNG expects, scales the dirty
// parts up into the kept frame and gives back the frame, its size and the dirty rects to pass on.
// Returns 0 when the frame is to be passed on as it is.
static BOOL UiScaleFrame(void* self, void* browser, int type, size_t* dirty_count, const int32_t** dirty,
                         const void** buffer, int* width, int* height) {
  float factor = g_ui_factor;
  if (factor >= 1.0f || !*buffer || *width <= 0 || *height <= 0 || (type != 0 && type != 1)) return 0;
  // The size CEF would draw at BeamNG's own scale (ScaleToCeiledSize): of the view, or of the popup.
  int size[4] = {0, 0, g_ui_popup[0], g_ui_popup[1]};
  if (type == 0) UiViewRect(self, browser, size);  // before the game's OnPaint, which releases the browser
  float scale = g_ui_game_scale;
  int to_width = (int)((float)size[2] * scale + 0.999f), to_height = (int)((float)size[3] * scale + 0.999f);
  if (type == 1 && (to_width <= 0 || to_height <= 0))
    to_width = (int)((float)*width / factor + 0.5f), to_height = (int)((float)*height / factor + 0.5f);
  if (to_width <= *width || to_height <= *height || to_width > 7680 || to_height > 4320) return 0;
  uint32_t bytes = (uint32_t)to_width * (uint32_t)to_height * 4;
  if (g_ui_frame_bytes[type] < bytes) {
    if (g_ui_frame[type]) VirtualFree(g_ui_frame[type], 0, 0x8000 /* MEM_RELEASE */);
    g_ui_frame[type] = VirtualAlloc(NULL, bytes, 0x3000, PAGE_READWRITE);
    g_ui_frame_bytes[type] = g_ui_frame[type] ? bytes : 0;
    if (!g_ui_frame[type]) return 0;
  }
  // All of it after a change of size, or with more rects than kept.
  int* sizes = g_ui_sizes[type];
  BOOL whole = sizes[0] != *width || sizes[1] != *height || sizes[2] != to_width || sizes[3] != to_height;
  sizes[0] = *width, sizes[1] = *height, sizes[2] = to_width, sizes[3] = to_height;
  size_t count = *dirty_count;
  const int32_t* from_rects = *dirty;
  if (whole || !from_rects || count > 64) count = 0;
  int32_t* rects = g_ui_rects[type];
  // Each pixel drawn blends the source pixels around it: the rects grow by a pixel and the ratio.
  int grow_x = to_width / *width + 1, grow_y = to_height / *height + 1;
  for (size_t i = 0; i < (count ? count : 1); ++i) {
    int x = count ? from_rects[4 * i] : 0, y = count ? from_rects[4 * i + 1] : 0;
    int w = count ? from_rects[4 * i + 2] : *width, h = count ? from_rects[4 * i + 3] : *height;
    int x0 = (int)((int64_t)x * to_width / *width) - grow_x, y0 = (int)((int64_t)y * to_height / *height) - grow_y;
    int x1 = (int)(((int64_t)(x + w) * to_width + *width - 1) / *width) + grow_x;
    int y1 = (int)(((int64_t)(y + h) * to_height + *height - 1) / *height) + grow_y;
    x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > to_width ? to_width : x1, y1 = y1 > to_height ? to_height : y1;
    if (x1 <= x0 || y1 <= y0) x0 = y0 = x1 = y1 = 0;
    else UiScaleRegion(*buffer, *width, *height, g_ui_frame[type], to_width, to_height, x0, y0, x1, y1);
    rects[4 * i] = x0, rects[4 * i + 1] = y0, rects[4 * i + 2] = x1 - x0, rects[4 * i + 3] = y1 - y0;
  }
  *dirty_count = count ? count : 1;
  *dirty = rects;
  *buffer = g_ui_frame[type];
  *width = to_width;
  *height = to_height;
  return 1;
}

// The shared memory libcef_real.dll maps, by view.
typedef void* (WINAPI* map_view_fn)(HANDLE, DWORD, DWORD, DWORD, size_t);
typedef void* (WINAPI* map_view_ex_fn)(HANDLE, DWORD, DWORD, DWORD, size_t, void*);
typedef BOOL (WINAPI* unmap_view_fn)(const void*);
static map_view_fn g_real_map_view;
static map_view_ex_fn g_real_map_view_ex;
static unmap_view_fn g_real_unmap_view;
enum { kViews = 16384 };
static struct {
  volatile long lock;
  uint64_t address[kViews];  // open addressing, 0 for none
  uint32_t kib[kViews];
  long views, peak_views, failed;
  uint64_t kib_total, peak_kib, biggest_kib;
} g_views;

static void ViewsLock(void) {
  while (__sync_lock_test_and_set(&g_views.lock, 1)) Sleep(0);
}
static void ViewsUnlock(void) { __sync_lock_release(&g_views.lock); }

static void NoteView(const void* address, BOOL mapped) {
  uint64_t key = (uint64_t)(uintptr_t)address;
  uint32_t kib = 0;
  if (mapped) {
    struct { void* base; void* allocation; DWORD allocation_protect; size_t size; DWORD state, protect, type; } region;
    if (VirtualQuery(address, &region, sizeof(region))) kib = (uint32_t)(region.size >> 10);
  }
  ViewsLock();
  size_t slot = (size_t)(key >> 16) % kViews;
  for (size_t probe = 0; probe < 256; ++probe, slot = (slot + 1) % kViews) {
    if (mapped && (!g_views.address[slot] || g_views.address[slot] == 1)) {
      g_views.address[slot] = key;
      g_views.kib[slot] = kib;
      ++g_views.views;
      g_views.kib_total += kib;
      if (g_views.views > g_views.peak_views) g_views.peak_views = g_views.views;
      if (g_views.kib_total > g_views.peak_kib) g_views.peak_kib = g_views.kib_total;
      if (kib > g_views.biggest_kib) g_views.biggest_kib = kib;
      break;
    }
    if (!mapped && g_views.address[slot] == key) {
      g_views.address[slot] = 1;  // a tombstone, so later keys of the chain are still found
      --g_views.views;
      g_views.kib_total -= g_views.kib[slot];
      break;
    }
    if (!mapped && !g_views.address[slot]) break;
  }
  ViewsUnlock();
}

static void NoteFailedView(size_t bytes) {
  ViewsLock();
  long failed = ++g_views.failed;
  long views = g_views.views;
  uint64_t kib = g_views.kib_total;
  ViewsUnlock();
  if (failed <= 8)
    VkLog("ui: CEF could not map shared memory: bytes, its views then, KiB then", bytes, (uint64_t)views, kib);
}

static void* WINAPI MapViewHook(HANDLE section, DWORD access, DWORD high, DWORD low, size_t bytes) {
  void* view = g_real_map_view(section, access, high, low, bytes);
  if (view) NoteView(view, 1);
  else NoteFailedView(bytes);
  return view;
}

static void* WINAPI MapViewExHook(HANDLE section, DWORD access, DWORD high, DWORD low, size_t bytes, void* base) {
  void* view = g_real_map_view_ex(section, access, high, low, bytes, base);
  if (view) NoteView(view, 1);
  else NoteFailedView(bytes);
  return view;
}

static BOOL WINAPI UnmapViewHook(const void* view) {
  NoteView(view, 0);
  return g_real_unmap_view(view);
}

// From cef_initialize, once libcef_real.dll is loaded.
static void HookCefViews(HMODULE real) {
  g_real_map_view = (map_view_fn)PatchImport(real, "MapViewOfFile", (void*)MapViewHook);
  g_real_map_view_ex = (map_view_ex_fn)PatchImport(real, "MapViewOfFileEx", (void*)MapViewExHook);
  g_real_unmap_view = (unmap_view_fn)PatchImport(real, "UnmapViewOfFile", (void*)UnmapViewHook);
  if (!g_real_unmap_view && (g_real_map_view || g_real_map_view_ex))
    Log("libcef-shim: ui: CEF maps views but imports no UnmapViewOfFile; the count only grows\n");
  VkLog("ui: CEF's shared memory counted: MapViewOfFile, MapViewOfFileEx, UnmapViewOfFile",
        g_real_map_view != NULL, g_real_map_view_ex != NULL, g_real_unmap_view != NULL);
}

// Every 10 s, from the watchdog's thread.
static void LogCefViews(void) {
  if (!g_real_map_view && !g_real_map_view_ex) return;
  ViewsLock();
  long views = g_views.views, peak_views = g_views.peak_views, failed = g_views.failed;
  uint64_t kib = g_views.kib_total, peak = g_views.peak_kib, biggest = g_views.biggest_kib;
  ViewsUnlock();
  char line[300];
  size_t n = SampleAppend(line, 0, sizeof(line), "libcef-shim: ui: CEF's shared memory: ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)views);
  n = SampleAppend(line, n, sizeof(line), " views, ");
  n = SampleNumber(line, n, sizeof(line), kib >> 10);
  n = SampleAppend(line, n, sizeof(line), " MiB; at most ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)peak_views);
  n = SampleAppend(line, n, sizeof(line), " views, ");
  n = SampleNumber(line, n, sizeof(line), peak >> 10);
  n = SampleAppend(line, n, sizeof(line), " MiB; the biggest ");
  n = SampleNumber(line, n, sizeof(line), biggest);
  n = SampleAppend(line, n, sizeof(line), " KiB; ");
  n = SampleNumber(line, n, sizeof(line), (uint64_t)failed);
  SampleAppend(line, n, sizeof(line), " could not be mapped\n");
  Log(line);
}
