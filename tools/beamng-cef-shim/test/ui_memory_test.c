// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test of ui_memory.c, built with the host's C compiler: the scale CEF is given, the frames
// scaled up from what CEF drew (against the source pixels, and each change against a whole new
// scale), the rects passed on with them, and the references to the browser, which each callback
// releases as CEF's C++ wrappers do. Prints the time a whole 1280x720 frame takes to scale to
// 1920x1080. run.sh runs it.
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
typedef int BOOL;
typedef uint32_t DWORD;
typedef void* HANDLE;
typedef void* HMODULE;
#define WINAPI
#define PAGE_READWRITE 4
static uint32_t g_ui_width_option = 1280;
static void VkLog(const char* what, uint64_t a, uint64_t b, uint64_t c) { (void)what, (void)a, (void)b, (void)c; }
static void Log(const char* text) { (void)text; }
static void* VirtualAlloc(void* at, size_t bytes, DWORD type, DWORD protect) {
  (void)at, (void)type, (void)protect;
  return calloc(1, bytes);
}
static BOOL VirtualFree(void* p, size_t bytes, DWORD type) {
  (void)bytes, (void)type;
  free(p);
  return 1;
}
static size_t VirtualQuery(const void* p, void* info, size_t size) { (void)p, (void)info, (void)size; return 0; }
static void Sleep(DWORD ms) { (void)ms; }
static void* PatchImport(HMODULE m, const char* name, void* with) { (void)m, (void)name, (void)with; return NULL; }
#include "../ui_memory.c"
static size_t SampleAppend(char* line, size_t n, size_t capacity, const char* text) {
  (void)line, (void)capacity;
  return n + strlen(text);
}
static size_t SampleNumber(char* line, size_t n, size_t capacity, uint64_t value) {
  (void)line, (void)capacity, (void)value;
  return n + 1;
}

static int failures;
#define CHECK(c, ...)                                     \
  do {                                                    \
    if (!(c)) {                                           \
      ++failures;                                         \
      printf("FAILED %s:%d: ", __FILE__, __LINE__);       \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
    }                                                     \
  } while (0)

// The browser: CEF adds a reference for each callback, and the game's callback releases it.
static long g_refs;
static void BrowserAddRef(void* self) { (void)self, ++g_refs; }
static int BrowserRelease(void* self) {
  (void)self;
  CHECK(--g_refs >= 0, "the browser was released more often than it was referenced");
  return g_refs == 0;
}
static void* g_browser[5] = {(void*)40, (void*)BrowserAddRef, (void*)BrowserRelease, NULL, NULL};

// The game's render handler: its view, and its own scale (when it fills the screen info).
static int g_view_width = 1920, g_view_height = 1080, g_game_fills = 1;
static float g_game_scale = 1.0f;
static void GameViewRect(void* self, void* browser, int* rect) {
  (void)self;
  rect[0] = rect[1] = 0, rect[2] = g_view_width, rect[3] = g_view_height;
  BrowserRelease(browser);
}
static int GameScreenInfo(void* self, void* browser, unsigned char* info) {
  (void)self;
  if (g_game_fills) *(float*)(info + 8) = g_game_scale;
  BrowserRelease(browser);
  return g_game_fills;
}
static int g_game_popup[4];
static void GamePopupSize(void* self, void* browser, const int* rect) {
  (void)self;
  memcpy(g_game_popup, rect, sizeof(g_game_popup));
  BrowserRelease(browser);
}
static void* g_handler[14];

// CEF asking for the screen info: its defaults, and a reference for the call.
static float CefScale(void) {
  unsigned char info[56] = {0};
  *(size_t*)info = sizeof(info);
  *(float*)(info + 8) = 1.0f;
  ++g_refs;
  GetScreenInfoHook(g_handler, g_browser, info);
  return *(float*)(info + 8);
}

// A paint as CEF makes it, passed through UiScaleFrame as OnPaint does; the game then releases.
static BOOL Paint(int type, const int32_t* rect, const uint32_t** buffer, int* width, int* height,
                  const int32_t** rects) {
  size_t count = 1;
  const void* pixels = *buffer;
  *rects = rect;
  ++g_refs;
  BOOL scaled = UiScaleFrame(g_handler, g_browser, type, &count, rects, &pixels, width, height);
  BrowserRelease(g_browser);
  CHECK(count == 1, "%zu rects passed on", count);
  *buffer = pixels;
  return scaled;
}

static uint32_t Noise(uint32_t seed, int x, int y) {
  uint32_t v = seed * 2654435761u ^ (uint32_t)x * 40503u ^ (uint32_t)y * 2246822519u;
  v ^= v >> 13;
  v *= 0x5bd1e995u;
  return v ^ (v >> 15);
}

// Random changes to a frame of w x h scaled to tw x th: after each one the kept frame must hold
// what a whole new scale gives, and every pixel that changed must be in the rect passed on.
static void ScaleChanges(int w, int h, int tw, int th) {
  g_view_width = tw, g_view_height = th;
  g_ui_factor = (float)w / (float)tw;
  memset(g_ui_sizes, 0, sizeof(g_ui_sizes));
  uint32_t* source = malloc((size_t)w * h * 4);
  uint32_t* whole = malloc((size_t)tw * th * 4);
  uint32_t* before = malloc((size_t)tw * th * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) source[y * w + x] = Noise(1, x, y);
  srand(7);
  for (int round = 0; round < 40; ++round) {
    int32_t rect[4] = {0, 0, w, h};
    if (round) {
      rect[0] = rand() % w, rect[1] = rand() % h;
      rect[2] = 1 + rand() % (round % 3 ? w - rect[0] : 5 < w - rect[0] ? 5 : w - rect[0]);
      rect[3] = 1 + rand() % (round % 3 ? h - rect[1] : 5 < h - rect[1] ? 5 : h - rect[1]);
      for (int y = rect[1]; y < rect[1] + rect[3]; ++y)
        for (int x = rect[0]; x < rect[0] + rect[2]; ++x) source[y * w + x] = Noise((uint32_t)round + 2, x, y);
      memcpy(before, g_ui_frame[0], (size_t)tw * th * 4);
    }
    const uint32_t* frame = source;
    const int32_t* passed;
    int width = w, height = h;
    BOOL scaled = Paint(0, rect, &frame, &width, &height, &passed);
    CHECK(scaled && width == tw && height == th, "%dx%d: scaled %d to %dx%d", w, h, scaled, width, height);
    if (!scaled) break;
    UiScaleRegion(source, w, h, whole, tw, th, 0, 0, tw, th);
    long wrong = 0, outside = 0;
    for (int y = 0; y < th; ++y)
      for (int x = 0; x < tw; ++x) {
        size_t i = (size_t)y * tw + x;
        BOOL inside = x >= passed[0] && x < passed[0] + passed[2] && y >= passed[1] && y < passed[1] + passed[3];
        wrong += frame[i] != whole[i];
        outside += round && !inside && frame[i] != before[i];
      }
    CHECK(!wrong, "%dx%d to %dx%d, change %d: %ld pixels differ from a whole scale", w, h, tw, th, round, wrong);
    CHECK(!outside, "%dx%d to %dx%d, change %d: %ld changed pixels outside the rect", w, h, tw, th, round, outside);
  }
  free(source), free(whole), free(before);
}

static uint32_t Grey(uint32_t v) { return 0xff000000u | v << 16 | v << 8 | v; }

int main(void) {
  g_handler[0] = (void*)(size_t)0x70;
  g_handler[0x38 / 8] = (void*)GameViewRect;
  g_game_get_screen_info = GameScreenInfo;

  // The scale given to CEF, and the size CEF then draws at (ScaleToCeiledSize, in float).
  static const struct {
    int width, height, fills;
    float game;
    uint32_t option;
    int drawn_width, drawn_height;
  } scales[] = {
      {1920, 1080, 1, 1.0f, 1280, 1280, 720}, {1920, 1080, 0, 1.0f, 1280, 1280, 720},
      {3840, 2160, 1, 1.0f, 1280, 1280, 720}, {2560, 1440, 1, 1.0f, 1280, 1280, 720},
      {1920, 1080, 1, 1.25f, 1280, 1280, 720}, {1280, 720, 1, 1.0f, 1280, 1280, 720},
      {1024, 768, 1, 1.0f, 1280, 1024, 768}, {1920, 1080, 1, 1.0f, 1600, 1600, 900},
      {1920, 1080, 1, 1.0f, 0, 1920, 1080},
  };
  for (size_t i = 0; i < sizeof(scales) / sizeof(scales[0]); ++i) {
    g_view_width = scales[i].width, g_view_height = scales[i].height;
    g_game_fills = scales[i].fills, g_game_scale = scales[i].game, g_ui_width_option = scales[i].option;
    float scale = CefScale();
    int width = (int)ceilf((float)scales[i].width * scale), height = (int)ceilf((float)scales[i].height * scale);
    CHECK(width == scales[i].drawn_width && height == scales[i].drawn_height, "view %dx%d, option %u: drawn %dx%d",
          scales[i].width, scales[i].height, scales[i].option, width, height);
  }
  CHECK(!g_refs, "screen info: %ld browser references left", g_refs);
  g_ui_width_option = 1280, g_game_fills = 1, g_game_scale = 1.0f;

  // Two pixels to three: the corners of each 3 x 3 block are source pixels, the rest averages.
  {
    static uint32_t source[1280 * 720];
    for (int i = 0; i < 1280 * 720; ++i) source[i] = Noise(5, i % 1280, i / 1280);
    g_view_width = 1920, g_view_height = 1080, g_ui_factor = 1279.75f / 1920.0f;
    memset(g_ui_sizes, 0, sizeof(g_ui_sizes));
    int32_t rect[4] = {0, 0, 1280, 720};
    const uint32_t* frame = source;
    const int32_t* passed;
    int width = 1280, height = 720;
    CHECK(Paint(0, rect, &frame, &width, &height, &passed) && width == 1920 && height == 1080, "1080p not scaled");
    long wrong = 0;
    for (int j = 0; j < 360; ++j)
      for (int i = 0; i < 640; ++i) {
        const uint32_t* a = source + (size_t)(2 * j) * 1280 + 2 * i;
        const uint32_t* b = a + 1280;
        const uint32_t* row = frame + (size_t)(3 * j) * 1920 + 3 * i;
        wrong += row[0] != a[0] || row[2] != a[1] || row[2 * 1920] != b[0] || row[2 * 1920 + 2] != b[1];
        wrong += row[1] != UiAverage(a[0], a[1]) || row[1920] != UiAverage(a[0], b[0]);
      }
    CHECK(!wrong, "1080p: %ld blocks not made from their source pixels", wrong);
  }

  // Bilinear on a ramp: each pixel within a level of the ramp at its centre.
  {
    static uint32_t source[256 * 4];
    for (int i = 0; i < 256 * 4; ++i) source[i] = Grey((uint32_t)(i % 256));
    g_view_width = 640, g_view_height = 10, g_ui_factor = 0.4f;
    memset(g_ui_sizes, 0, sizeof(g_ui_sizes));
    int32_t rect[4] = {0, 0, 256, 4};
    const uint32_t* frame = source;
    const int32_t* passed;
    int width = 256, height = 4;
    CHECK(Paint(0, rect, &frame, &width, &height, &passed) && width == 640 && height == 10, "ramp not scaled");
    int worst = 0;
    for (int x = 0; x < 640; ++x) {
      double at = (x + 0.5) * 256.0 / 640.0 - 0.5;
      double want = at < 0 ? 0 : at > 255 ? 255 : at;
      int off = abs((int)(frame[5 * 640 + x] & 0xff) - (int)lround(want));
      worst = off > worst ? off : worst;
    }
    CHECK(worst <= 1, "ramp: a pixel %d levels off", worst);
  }

  // Each change against a whole new scale: 1080p, 4K, 1440p, other widths and a small option.
  static const int sizes[][4] = {{1280, 720, 1920, 1080}, {1280, 720, 3840, 2160}, {1280, 720, 2560, 1440},
                                 {1600, 900, 1920, 1080}, {1280, 720, 1366, 768},  {640, 360, 1920, 1080}};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
    ScaleChanges(sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3]);

  // A change of size: all of the frame is scaled again and passed on, whatever the rect says.
  {
    static uint32_t source[1280 * 720], whole[2560 * 1440];
    for (int i = 0; i < 1280 * 720; ++i) source[i] = Noise(11, i % 1280, i / 1280);
    g_view_width = 1920, g_view_height = 1080, g_ui_factor = 1279.75f / 1920.0f;
    memset(g_ui_sizes, 0, sizeof(g_ui_sizes));
    int32_t all[4] = {0, 0, 1280, 720}, part[4] = {10, 10, 4, 4};
    const uint32_t* frame = source;
    const int32_t* passed;
    int width = 1280, height = 720;
    Paint(0, all, &frame, &width, &height, &passed);
    g_view_width = 2560, g_view_height = 1440, g_ui_factor = 1279.75f / 2560.0f;
    frame = source, width = 1280, height = 720;
    CHECK(Paint(0, part, &frame, &width, &height, &passed) && width == 2560 && height == 1440, "1440p not scaled");
    UiScaleRegion(source, 1280, 720, whole, 2560, 1440, 0, 0, 2560, 1440);
    CHECK(!memcmp(frame, whole, sizeof(whole)), "after a change of size, the frame is not all scaled again");
    CHECK(passed[0] == 0 && passed[1] == 0 && passed[2] == 2560 && passed[3] == 1440,
          "after a change of size, the rect passed on is %d,%d %dx%d", passed[0], passed[1], passed[2], passed[3]);
  }

  // Passed on as they are: at a factor of 1, and a frame already at the view's size.
  {
    static uint32_t source[16];
    int32_t rect[4] = {0, 0, 4, 4};
    const uint32_t* frame = source;
    const int32_t* passed;
    int width = 4, height = 4;
    g_ui_factor = 1.0f;
    CHECK(!Paint(0, rect, &frame, &width, &height, &passed) && frame == source && width == 4, "scaled at 1");
    g_ui_factor = 0.5f, g_view_width = 4, g_view_height = 4;
    CHECK(!Paint(0, rect, &frame, &width, &height, &passed) && frame == source && width == 4, "scaled to its size");
  }

  // A popup (a <select> list): scaled to the size OnPopupSize gave, or by the factor before that.
  {
    static uint32_t source[200 * 100];
    int32_t rect[4] = {0, 0, 200, 100};
    const uint32_t* frame = source;
    const int32_t* passed;
    int width = 200, height = 100;
    g_ui_factor = 1279.75f / 1920.0f;
    g_view_width = 0;
    BOOL scaled = Paint(1, rect, &frame, &width, &height, &passed);
    CHECK(scaled && width == 300 && height == 150, "popup: scaled %d to %dx%d", scaled, width, height);
    int popup[4] = {40, 30, 200, 101};  // in the view; CEF draws it at 134x68
    g_game_on_popup_size = GamePopupSize;
    ++g_refs;
    OnPopupSizeHook(g_handler, g_browser, popup);
    int32_t drawn[4] = {0, 0, 134, 68};
    frame = source, width = 134, height = 68;
    scaled = Paint(1, drawn, &frame, &width, &height, &passed);
    CHECK(scaled && width == 200 && height == 101 && g_game_popup[2] == 200, "popup: scaled %d to %dx%d", scaled,
          width, height);
  }
  CHECK(!g_refs, "paints: %ld browser references left", g_refs);

  // The time of a whole frame, 1280x720 to 1920x1080.
  {
    static uint32_t source[1280 * 720];
    g_view_width = 1920, g_view_height = 1080, g_ui_factor = 1279.75f / 1920.0f;
    int32_t rect[4] = {0, 0, 1280, 720};
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (int k = 0; k < 30; ++k) {
      const uint32_t* frame = source;
      const int32_t* passed;
      int width = 1280, height = 720;
      Paint(0, rect, &frame, &width, &height, &passed);
    }
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = ((double)(end.tv_sec - start.tv_sec) * 1e3 + (double)(end.tv_nsec - start.tv_nsec) / 1e6) / 30;
    printf("ui_memory: a whole 1280x720 frame scaled to 1920x1080 in %.2f ms\n", ms);
  }
  printf(failures ? "ui_memory: FAILED\n" : "ui_memory: test: done\n");
  return failures != 0;
}
