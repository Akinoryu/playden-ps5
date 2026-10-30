// SPDX-License-Identifier: GPL-3.0-or-later
//
// Swapchain watch for the PS5 display, included by shim.c.
//
// Every winevulkan call goes through one pointer, the Unix call dispatcher.
// The shim puts its own function there, logs swapchain calls (create, destroy,
// acquire, present) and passes the rest on unchanged. Swapchains are made at
// the full size with the usage the overlay needs (see the create below), a
// destroyed one is kept for the identical create that follows, and presents
// go through the overlay (overlay.c).
//
// On the PS5, swapchains of one size share their display buffers, and a
// replaced swapchain that still holds an image the game acquired keeps that
// buffer busy until it is destroyed or presented. Two of them and the buffer
// on screen leave nothing to acquire, and an acquire without a timeout waits
// for good. So an acquire waits in one-second steps, and while it is stuck,
// images still held on replaced swapchains are presented there, which gives
// their buffers back (the present of a replaced swapchain is refused as out of
// date). shim:no-vk-fix turns that off; the log stays.

typedef long (*unix_call_fn)(uint64_t handle, unsigned int code, void* args);

enum { kMaxChains = 16, kCallLogLimit = 120 };

typedef struct {
  uint64_t handle;
  uint32_t width, height, image_count;
  BOOL retired;
  uint32_t acquired;  // bit per image index: acquired and not presented
  // Reuse: the game destroyed it and the shim kept it for an identical create.
  BOOL parked;
  uint32_t orphaned;  // images the game held when it destroyed it, presented once a queue is known
  uint64_t surface;
  uint32_t format, color_space, usage, present_mode;
  uint64_t device;
  BOOL enlarged;  // made at the full size; the overlay scales the game's frames up to it
} chain_t;

static unix_call_fn g_real_dispatcher;
static unix_call_fn* g_dispatcher_slot;
static unsigned g_code_acquire = ~0u, g_code_acquire2 = ~0u, g_code_create = ~0u, g_code_destroy = ~0u,
                g_code_present = ~0u;
static chain_t g_chains[kMaxChains];
static uint64_t g_current_chain;
static uint64_t g_last_queue;
static volatile long g_vk_lock;
static long g_vk_logged;
static BOOL g_vk_fix = 1;
static BOOL g_vk_reuse = 1;
static int32_t (*g_queue_present)(uint64_t queue, const void* info);
static uint64_t g_present_handle;

// One loaded winevulkan.dll: its unixlib handle cell and its vkQueuePresentKHR.
typedef struct {
  HMODULE module;
  const uint64_t* handle_cell;
  int32_t (*present)(uint64_t queue, const void* info);
  unix_call_fn real;
  unix_call_fn* slot;
  unsigned char* sites[16];
  unsigned char site_bytes[16][6];
  int site_count;
} vk_module_t;

static vk_module_t g_vk_modules[4];
static int g_vk_module_count;

// The dispatcher of the copy whose unixlib handle a call carries.
static unix_call_fn RealFor(uint64_t handle) {
  for (int i = 0; i < g_vk_module_count; ++i)
    if (g_vk_modules[i].handle_cell && *g_vk_modules[i].handle_cell == handle) return g_vk_modules[i].real;
  return g_real_dispatcher;
}

static void VkLock(void) {
  while (__sync_lock_test_and_set(&g_vk_lock, 1)) Sleep(0);
}
static void VkUnlock(void) { __sync_lock_release(&g_vk_lock); }

static chain_t* FindChain(uint64_t handle) {
  for (int i = 0; i < kMaxChains; ++i)
    if (g_chains[i].handle == handle && handle) return &g_chains[i];
  return NULL;
}

// "libcef-shim: vk <what> <a> <b> <c>" with numbers in hex.
static void VkLog(const char* what, uint64_t a, uint64_t b, uint64_t c) {
  char line[200];
  size_t n = 0;
  const char* head = "libcef-shim: vk ";
  while (*head) line[n++] = *head++;
  while (*what && n < 120) line[n++] = *what++;
  uint64_t values[3] = {a, b, c};
  for (int k = 0; k < 3; ++k) {
    char digits[20];
    int d = 0;
    uint64_t v = values[k];
    do digits[d++] = "0123456789abcdef"[v & 15], v >>= 4; while (v && d < 20);
    line[n++] = ' ';
    line[n++] = '0';
    line[n++] = 'x';
    while (d) line[n++] = digits[--d];
  }
  line[n++] = '\n';
  line[n] = 0;
  Log(line);
}

static void LogChains(const char* why) {
  VkLog(why, g_current_chain, 0, 0);
  for (int i = 0; i < kMaxChains; ++i)
    if (g_chains[i].handle)
      VkLog(g_chains[i].retired ? "  chain retired, acquired" : "  chain current, acquired", g_chains[i].handle,
            g_chains[i].acquired, ((uint64_t)g_chains[i].width << 32) | g_chains[i].height);
}

// VkPresentInfoKHR, by offset.
typedef struct {
  uint32_t s_type;
  const void* next;
  uint32_t wait_count;
  const void* waits;
  uint32_t swapchain_count;
  const uint64_t* swapchains;
  const uint32_t* indices;
  int32_t* results;
} present_info_t;

// overlay.c
static const present_info_t* OverlayPresent(void* queue, const present_info_t* info);
static void OverlayNoteQueue(void* queue, uint32_t family);
static void OverlayNotePhysical(void* physical);
static void OverlayNoteSwapchain(void* device, uint64_t swapchain, uint32_t width, uint32_t height,
                                 uint32_t game_width, uint32_t game_height, uint32_t format);
static void OverlayForgetSwapchain(uint64_t swapchain);
static BOOL OverlayCanScale(uint32_t width, uint32_t height, uint32_t format);
static uint32_t g_full_width, g_full_height;
static unsigned g_code_get_queue = ~0u, g_code_get_queue2 = ~0u, g_code_surface_caps = ~0u;
// Calls whose first parameter is the physical device the overlay needs (memory properties and the
// like, which BeamNG asks for when it starts).
static unsigned g_code_physical[3] = {~0u, ~0u, ~0u};

// Presents every image still acquired on a replaced swapchain, which frees its
// display buffer. Called with the lock not held. Goes through winevulkan's own
// vkQueuePresentKHR, which does more around the Unix call than the call itself.
static BOOL ReleaseRetiredImages(void) {
  uint64_t chains[kMaxChains];
  uint32_t indices[kMaxChains];
  uint32_t count = 0;
  VkLock();
  for (int i = 0; i < kMaxChains && count < kMaxChains; ++i) {
    chain_t* chain = &g_chains[i];
    if (!chain->handle || !chain->retired || !chain->acquired) continue;
    for (uint32_t index = 0; index < 32 && count < kMaxChains; ++index)
      if (chain->acquired & (1u << index)) {
        chains[count] = chain->handle;
        indices[count] = index;
        ++count;
      }
  }
  uint64_t queue = g_last_queue;
  int32_t (*present)(uint64_t, const void*) = g_queue_present;
  for (int i = 0; i < g_vk_module_count; ++i)
    if (g_vk_modules[i].handle_cell && *g_vk_modules[i].handle_cell == g_present_handle)
      present = g_vk_modules[i].present;
  VkUnlock();
  if (!count || !queue || !present) return 0;
  for (uint32_t i = 0; i < count; ++i) {
    present_info_t info = {1000001001 /* VK_STRUCTURE_TYPE_PRESENT_INFO_KHR */, NULL, 0, NULL, 1, &chains[i],
                           &indices[i], NULL};
    int32_t result = present(queue, &info);
    VkLog("released stuck image: chain, index, result", chains[i], indices[i], (uint32_t)result);
    VkLock();
    chain_t* chain = FindChain(chains[i]);
    if (chain) chain->acquired &= ~(1u << indices[i]);
    VkUnlock();
  }
  return 1;
}

// Presents the images the game held when it destroyed a swapchain the shim kept, which frees
// their display buffers. Needs a queue, so it runs after the game's first present on that chain.
static void PresentOrphans(uint64_t swapchain) {
  uint32_t indices[32];
  uint32_t count = 0;
  VkLock();
  chain_t* chain = FindChain(swapchain);
  if (chain && chain->orphaned) {
    for (uint32_t index = 0; index < 32; ++index)
      if (chain->orphaned & (1u << index)) indices[count++] = index;
    chain->orphaned = 0;
  }
  uint64_t queue = g_last_queue;
  int32_t (*present)(uint64_t, const void*) = g_queue_present;
  for (int i = 0; i < g_vk_module_count; ++i)
    if (g_vk_modules[i].handle_cell && *g_vk_modules[i].handle_cell == g_present_handle)
      present = g_vk_modules[i].present;
  VkUnlock();
  for (uint32_t i = 0; i < count && queue && present; ++i) {
    present_info_t info = {1000001001, NULL, 0, NULL, 1, &swapchain, &indices[i], NULL};
    int32_t result = present(queue, &info);
    VkLog("gave back an image held at destroy: chain, index, result", swapchain, indices[i], (uint32_t)result);
  }
}

static long g_presents;

static void NotePresent(uint64_t queue, const present_info_t* info, int32_t result) {
  VkLock();
  if (++g_presents % 600 == 0) VkLog("presents so far, latest result", g_presents, (uint32_t)result, 0);
  g_last_queue = queue;
  for (uint32_t i = 0; info && i < info->swapchain_count; ++i) {
    chain_t* chain = FindChain(info->swapchains[i]);
    if (chain && info->indices[i] < 32) chain->acquired &= ~(1u << info->indices[i]);
    if (g_vk_logged < kCallLogLimit || result != 0) {
      ++g_vk_logged;
      VkLog("present: chain, index, result", info->swapchains[i], info->indices[i], (uint32_t)result);
    }
  }
  VkUnlock();
}

static void NoteAcquire(uint64_t swapchain, uint32_t index, int32_t result) {
  VkLock();
  chain_t* chain = FindChain(swapchain);
  if (chain && (result == 0 || result == 1000001003 /* SUBOPTIMAL */) && index < 32) chain->acquired |= 1u << index;
  if (g_vk_logged < kCallLogLimit || (result != 0 && result != 2 /* TIMEOUT */)) {
    ++g_vk_logged;
    VkLog("acquire: chain, index, result", swapchain, index, (uint32_t)result);
  }
  VkUnlock();
}

// An acquire with a long timeout waits in one-second steps. An acquire that
// times out, in steps or because the game asked for a short timeout and tries
// again, counts as stuck once a swapchain has had no image for a second; then
// the swapchains are logged and, with the fix on, images held on replaced
// swapchains are presented to free their buffers.
static uint64_t g_stuck_chain, g_stuck_since_ms, g_stuck_logged_ms;

static void NoteTimeout(uint64_t swapchain) {
  uint64_t now = GetTickCount64();
  VkLock();
  if (g_stuck_chain != swapchain) {
    g_stuck_chain = swapchain;
    g_stuck_since_ms = now;
    g_stuck_logged_ms = 0;
  }
  uint64_t stuck_ms = now - g_stuck_since_ms;
  BOOL log = stuck_ms >= 1000 && (!g_stuck_logged_ms || now - g_stuck_logged_ms >= 10000);
  if (log) {
    g_stuck_logged_ms = now;
    VkLog("acquire stuck: chain, milliseconds", swapchain, stuck_ms, 0);
    LogChains("swapchains, current");
  }
  VkUnlock();
  if (stuck_ms >= 1000 && g_vk_fix) ReleaseRetiredImages();
}

static void NoteAcquired(void) {
  VkLock();
  if (g_stuck_chain && g_stuck_logged_ms) VkLog("acquire works again: chain", g_stuck_chain, 0, 0);
  g_stuck_chain = 0;
  VkUnlock();
}

static long Acquire(uint64_t handle, unsigned code, void* args, uint64_t swapchain, uint64_t* timeout,
                    int32_t* result) {
  const uint64_t asked = *timeout;
  const uint64_t step = 1000000000ull;
  uint64_t waited = 0;
  for (;;) {
    *timeout = asked < step ? asked : step;
    long status = RealFor(handle)(handle, code, args);
    *timeout = asked;
    if (status) return status;
    if (*result != 2 /* VK_TIMEOUT */) {
      NoteAcquired();
      return status;
    }
    NoteTimeout(swapchain);
    if (asked <= step) return status;  // the game asked for a short wait; it will call again
    waited += step;
    if (asked != ~0ull && waited >= asked) return status;
  }
}

static volatile long g_vk_calls;

// Frame pacing, one line every 10 s from the present path: how many frames, how long they took from
// present to present, how many missed a 60 Hz refresh (over 17 ms) or a 30 Hz one (over 34 ms) or
// hitched (over 100 ms), and of each frame how long the game waited in vkAcquireNextImageKHR (for a
// display buffer to free up: the GPU and the refresh) and spent in vkQueuePresentKHR (with the
// overlay's copy), and how long CEF's paints took on the game's main thread (paint_hook.c).
static struct {
  int64_t frequency, last, window;
  long frames, over17, over34, over100, paints;
  int64_t longest, acquire, present, paint;
  int hitches;  // frames over 100 ms this window: when they began, since the shim loaded, and how long
  int64_t hitch_at[8], hitch_length[8];
} g_pace;

static int64_t PaceNow(void) {
  int64_t now = 0;
  QueryPerformanceCounter(&now);
  return now;
}

// Appends text, or ticks as milliseconds with one decimal, or a count.
static void PaceText(char* line, size_t* n, const char* text) {
  while (*text && *n < 380) line[(*n)++] = *text++;
}
static void PaceCount(char* line, size_t* n, uint64_t value) {
  char digits[24];
  int d = 0;
  do digits[d++] = (char)('0' + value % 10), value /= 10; while (value && d < 24);
  while (d && *n < 380) line[(*n)++] = digits[--d];
}
static void PaceMs(char* line, size_t* n, int64_t ticks) {
  uint64_t tenths = ticks > 0 ? (uint64_t)ticks * 10000 / (uint64_t)g_pace.frequency : 0;
  PaceCount(line, n, tenths / 10);
  PaceText(line, n, ".");
  PaceCount(line, n, tenths % 10);
}

// The calls of the thread that submits the game's frames, by kind, for a second pace line: waits
// for the GPU (fences, semaphores, idle queues and devices), queue submits, polls of fences and
// timeline semaphores, and every other call (recording, mostly). In BeamNG that thread makes about
// 8,700 calls a frame while another one only acquires and presents (build 21). Timed with rdtsc, a
// few nanoseconds where QueryPerformanceCounter may cross to the host, when rdtsc is that cheap
// (MeasureClocks), and turned into time with each window's ratio of the two. Acquires and presents,
// and the overlay's calls inside a present, are the present branch's to time (kVkOwn). Only that
// thread writes the counts, which only grow; each pace line takes what they grew by.
enum { kVkOther, kVkWait, kVkSubmit, kVkPoll, kVkOwn, kVkKinds, kVkCodes = 1024 };
static unsigned char g_call_kind[kVkCodes];
static struct {
  volatile DWORD thread;           // the thread that submits
  volatile DWORD presenter;        // the thread that presents
  int nested;                      // in the present branch
  BOOL timing;                     // rdtsc is cheap enough to time every call
  volatile uint64_t last_submit;   // GetTickCount64 at the timed thread's latest submit
  volatile uint64_t ticks[kVkKinds];
  volatile long calls[kVkKinds];
  uint64_t tsc, seen_ticks[kVkKinds];  // the present thread's, at the start of the window
  long all_calls, seen_calls[kVkKinds];
} g_vkt;

static uint64_t Tsc(void) { return __builtin_ia32_rdtsc(); }

// The timed thread: the first that submits, and another once it has not submitted for a second.
static void NoteSubmitter(DWORD thread) {
  uint64_t now = GetTickCount64();
  if (g_vkt.thread && now - g_vkt.last_submit < 1000) return;
  g_vkt.last_submit = now;
  g_vkt.thread = thread;
}

// What QueryPerformanceCounter and rdtsc cost here, logged once at start; every call of the timed
// thread is timed only when rdtsc takes under 200 ns.
static void MeasureClocks(void) {
  int64_t frequency = 0, before = 0, after = 0, value = 0;
  QueryPerformanceFrequency(&frequency);
  if (!frequency) return;
  QueryPerformanceCounter(&before);
  for (int i = 0; i < 10000; ++i) QueryPerformanceCounter(&value);
  QueryPerformanceCounter(&after);
  uint64_t qpc_ns = (uint64_t)(after - before) * 100000 / (uint64_t)frequency;  // per call: 1e9 / 10000
  volatile uint64_t sink = 0;
  QueryPerformanceCounter(&before);
  for (int i = 0; i < 100000; ++i) sink += Tsc();
  QueryPerformanceCounter(&after);
  uint64_t tsc_ns = (uint64_t)(after - before) * 10000 / (uint64_t)frequency;  // per call: 1e9 / 100000
  g_vkt.timing = tsc_ns < 200;
  char line[160];
  size_t n = 0;
  PaceText(line, &n, "libcef-shim: clocks: QueryPerformanceCounter takes ");
  PaceCount(line, &n, qpc_ns);
  PaceText(line, &n, " ns, rdtsc ");
  PaceCount(line, &n, tsc_ns);
  PaceText(line, &n, g_vkt.timing ? " ns\n" : " ns, too slow to time each Vulkan call\n");
  line[n] = 0;
  Log(line);
}

// The second pace line: per frame of the timed thread, the time and number of calls of each kind,
// the rest of the frame (the game's own work there) and the calls other threads made.
static void PaceCalls(int64_t now, long frames) {
  uint64_t tsc = Tsc(), elapsed = tsc - g_vkt.tsc;
  int64_t window = now - g_pace.window;
  long all_calls = g_vk_calls;
  uint64_t ticks[kVkKinds];
  long calls[kVkKinds];
  for (int k = 0; k < kVkKinds; ++k) {
    uint64_t t = g_vkt.ticks[k];
    long c = g_vkt.calls[k];
    ticks[k] = t - g_vkt.seen_ticks[k], calls[k] = c - g_vkt.seen_calls[k];
    g_vkt.seen_ticks[k] = t, g_vkt.seen_calls[k] = c;
  }
  DWORD thread = g_vkt.thread;
  // TSC ticks per QueryPerformanceCounter tick, times 16.
  uint64_t scale = window > 0 && (int64_t)elapsed > 0 ? elapsed * 16 / (uint64_t)window : 0;
  if (scale && thread) {
    int64_t spent[kVkKinds], vulkan = 0;
    for (int k = 0; k < kVkKinds; ++k) vulkan += spent[k] = g_vkt.timing ? (int64_t)(ticks[k] * 16 / scale) : 0;
    // The rest of that thread's frame; acquires and presents are part of its Vulkan time when it
    // makes them too.
    int64_t rest = window - vulkan - (thread == g_vkt.presenter ? g_pace.acquire + g_pace.present : 0);
    static const char* const label[] = {" ms in other Vulkan calls (", " ms waiting for the GPU (", " ms submitting (",
                                        " ms polling ("};
    static const int order[] = {kVkWait, kVkSubmit, kVkPoll, kVkOther};
    char line[400];
    size_t n = 0;
    PaceText(line, &n, "libcef-shim: pace: per frame on the thread that submits (");
    PaceCount(line, &n, thread);
    PaceText(line, &n, "): ");
    for (int i = 0; i < 4; ++i) {
      PaceMs(line, &n, spent[order[i]] / frames);
      PaceText(line, &n, label[order[i]]);
      PaceCount(line, &n, (uint64_t)((calls[order[i]] + frames / 2) / frames));
      PaceText(line, &n, i < 3 ? " calls), " : " calls); ");
    }
    if (g_vkt.timing) {
      PaceMs(line, &n, rest > 0 ? rest / frames : 0);
      PaceText(line, &n, " ms the game's own; ");
    }
    long others = all_calls - g_vkt.all_calls;
    PaceCount(line, &n, (uint64_t)(others > 0 ? (others + frames / 2) / frames : 0));
    PaceText(line, &n, " Vulkan calls on other threads\n");
    line[n] = 0;
    Log(line);
  }
  g_vkt.tsc = tsc;
  g_vkt.all_calls = all_calls;
}

// Seconds with one decimal.
static void PaceSeconds(char* line, size_t* n, int64_t ticks) {
  uint64_t tenths = ticks > 0 ? (uint64_t)ticks * 10 / (uint64_t)g_pace.frequency : 0;
  PaceCount(line, n, tenths / 10);
  PaceText(line, n, ".");
  PaceCount(line, n, tenths % 10);
}

// A third pace line when frames took over 100 ms: when each began, in seconds since the shim
// loaded (BeamNG's own log counts from about then), and how long it took, to see whether they
// come at a steady rhythm and what else happened then.
static void PaceHitches(void) {
  if (!g_pace.hitches) return;
  char line[400];
  size_t n = 0;
  PaceText(line, &n, "libcef-shim: pace: frames over 100 ms, at seconds (ms):");
  for (int i = 0; i < g_pace.hitches; ++i) {
    PaceText(line, &n, i ? ", " : " ");
    PaceSeconds(line, &n, g_pace.hitch_at[i]);
    PaceText(line, &n, " (");
    PaceCount(line, &n, (uint64_t)(g_pace.hitch_length[i] * 1000 / g_pace.frequency));
    PaceText(line, &n, ")");
  }
  PaceText(line, &n, "\n");
  line[n] = 0;
  Log(line);
}

static void PaceFrame(int64_t now) {
  if (!g_pace.frequency) {
    QueryPerformanceFrequency(&g_pace.frequency);
    g_pace.last = g_pace.window = now;
    for (int k = 0; k < kVkKinds; ++k) g_vkt.seen_ticks[k] = g_vkt.ticks[k], g_vkt.seen_calls[k] = g_vkt.calls[k];
    g_vkt.tsc = Tsc();
    g_vkt.all_calls = g_vk_calls;
    return;
  }
  int64_t interval = now - g_pace.last, ms = g_pace.frequency / 1000;
  g_pace.last = now;
  ++g_pace.frames;
  g_pace.over17 += interval > 17 * ms + ms / 2;
  g_pace.over34 += interval > 34 * ms;
  g_pace.over100 += interval > 100 * ms;
  if (interval > g_pace.longest) g_pace.longest = interval;
  if (interval > 100 * ms && g_pace.hitches < 8) {
    g_pace.hitch_at[g_pace.hitches] = now - interval - g_start_qpc;
    g_pace.hitch_length[g_pace.hitches++] = interval;
  }
  if (now - g_pace.window < 10 * g_pace.frequency || !g_pace.frames) return;
  char line[400];
  size_t n = 0;
  long frames = g_pace.frames, paints = g_pace.paints;
  PaceText(line, &n, "libcef-shim: pace: ");
  PaceCount(line, &n, (uint64_t)frames);
  PaceText(line, &n, " frames, ");
  PaceMs(line, &n, (now - g_pace.window) / frames);
  PaceText(line, &n, " ms each; ");
  PaceCount(line, &n, (uint64_t)g_pace.over17);
  PaceText(line, &n, " over 17 ms, ");
  PaceCount(line, &n, (uint64_t)g_pace.over34);
  PaceText(line, &n, " over 34, ");
  PaceCount(line, &n, (uint64_t)g_pace.over100);
  PaceText(line, &n, " over 100, longest ");
  PaceMs(line, &n, g_pace.longest);
  PaceText(line, &n, " ms; per frame ");
  PaceMs(line, &n, g_pace.acquire / frames);
  PaceText(line, &n, " ms in acquire, ");
  PaceMs(line, &n, g_pace.present / frames);
  PaceText(line, &n, " ms in present; ");
  PaceCount(line, &n, (uint64_t)paints);
  PaceText(line, &n, " UI paints, ");
  PaceMs(line, &n, paints ? g_pace.paint / paints : 0);
  PaceText(line, &n, " ms each\n");
  line[n] = 0;
  Log(line);
  PaceCalls(now, frames);
  PaceHitches();
  g_pace.window = now;
  g_pace.hitches = 0;
  g_pace.frames = g_pace.over17 = g_pace.over34 = g_pace.over100 = g_pace.paints = 0;
  g_pace.longest = g_pace.acquire = g_pace.present = g_pace.paint = 0;
}
static volatile long g_paints, g_accelerated_paints, g_blank_paints;  // paint_hook.c

// Wine answers VK_SUBOPTIMAL_KHR for a swapchain whose size is not its window's, and an enlarged
// one never is: the game would make its swapchain again at every frame. Its acquires and
// presents report success instead.
enum { kSuboptimal = 1000001003 };

static BOOL ChainEnlarged(uint64_t handle) {
  VkLock();
  chain_t* chain = FindChain(handle);
  BOOL enlarged = chain && chain->enlarged;
  VkUnlock();
  return enlarged;
}

static void HideSuboptimal(uint64_t swapchain, int32_t* result) {
  static long hidden;
  if (*result != kSuboptimal || !ChainEnlarged(swapchain)) return;
  *result = 0;
  if (++hidden == 1)
    VkLog("Wine's suboptimal answers for the enlarged swapchain hidden from the game", swapchain, 0, 0);
}

static long VkDispatch(uint64_t handle, unsigned int code, void* args) {
  if (code == g_code_acquire) {
    struct {
      uint64_t device, swapchain, timeout, semaphore, fence;
      uint32_t* index;
      int32_t result;
    }* p = args;
    int64_t start = PaceNow();
    long status = Acquire(handle, code, args, p->swapchain, &p->timeout, &p->result);
    g_pace.acquire += PaceNow() - start;
    if (!status) HideSuboptimal(p->swapchain, &p->result);
    if (!status) NoteAcquire(p->swapchain, p->index ? *p->index : 0, p->result);
    return status;
  }
  if (code == g_code_acquire2) {
    struct {
      uint64_t device;
      struct {
        uint32_t s_type;
        const void* next;
        uint64_t swapchain, timeout, semaphore, fence;
        uint32_t device_mask;
      }* info;
      uint32_t* index;
      int32_t result;
    }* p = args;
    int64_t start = PaceNow();
    long status = Acquire(handle, code, args, p->info->swapchain, &p->info->timeout, &p->result);
    g_pace.acquire += PaceNow() - start;
    if (!status) HideSuboptimal(p->info->swapchain, &p->result);
    if (!status) NoteAcquire(p->info->swapchain, p->index ? *p->index : 0, p->result);
    return status;
  }
  if (code == g_code_present) {
    struct {
      uint64_t queue;
      const present_info_t* info;
      int32_t result;
    }* p = args;
    const present_info_t* game_info = p->info;
    g_vkt.presenter = GetCurrentThreadId();
    g_vkt.nested = 1;
    int64_t start = PaceNow();
    const present_info_t* overlay_info = OverlayPresent((void*)(uintptr_t)p->queue, p->info);
    if (overlay_info) p->info = overlay_info;
    long status = RealFor(handle)(handle, code, args);
    p->info = game_info;
    int64_t end = PaceNow();
    g_vkt.nested = 0;
    g_pace.present += end - start;
    PaceFrame(end);
    g_present_handle = handle;
    if (!status && p->info && p->info->swapchain_count == 1) {
      if (p->info->results) HideSuboptimal(p->info->swapchains[0], &p->info->results[0]);
      HideSuboptimal(p->info->swapchains[0], &p->result);
    }
    NotePresent(p->queue, p->info, status ? -1 : p->result);
    for (uint32_t i = 0; !status && p->info && i < p->info->swapchain_count; ++i)
      PresentOrphans(p->info->swapchains[i]);
    return status;
  }
  if (code == g_code_create) {
    struct {
      uint64_t device;
      const unsigned char* info;  // VkSwapchainCreateInfoKHR
      const void* allocator;
      uint64_t* swapchain;
      int32_t result;
    }* p = args;
    uint32_t width = *(const uint32_t*)(p->info + 44), height = *(const uint32_t*)(p->info + 48);
    uint64_t old = *(const uint64_t*)(p->info + 96);
    uint64_t surface = *(const uint64_t*)(p->info + 24);
    uint32_t format = *(const uint32_t*)(p->info + 36), color_space = *(const uint32_t*)(p->info + 40);
    uint32_t usage = *(const uint32_t*)(p->info + 56), present_mode = *(const uint32_t*)(p->info + 88);
    // An identical swapchain to one the game just destroyed: hand back the one the shim kept.
    // On the PS5 each destroy and create of a same-size swapchain reuses its display buffers,
    // and BeamNG's third one never got an image.
    VkLock();
    for (int i = 0; i < kMaxChains; ++i) {
      chain_t* kept = &g_chains[i];
      if (!kept->handle || !kept->parked) continue;
      BOOL same = kept->device == p->device && kept->surface == surface && kept->width == width &&
                  kept->height == height && kept->format == format && kept->color_space == color_space &&
                  kept->usage == usage && kept->present_mode == present_mode && (!old || old == kept->handle);
      if (!same) continue;
      kept->parked = 0;
      kept->retired = 0;
      g_current_chain = kept->handle;
      *p->swapchain = kept->handle;
      p->result = 0;
      VkLog("create swapchain: reused the kept one, held images", kept->handle, kept->orphaned, 0);
      VkUnlock();
      return 0;
    }
    // Anything else: really destroy what the shim kept, then create.
    uint64_t kept_handles[kMaxChains], kept_devices[kMaxChains];
    int kept_count = 0;
    for (int i = 0; i < kMaxChains; ++i)
      if (g_chains[i].handle && g_chains[i].parked) {
        kept_handles[kept_count] = g_chains[i].handle;
        kept_devices[kept_count++] = g_chains[i].device;
        g_chains[i].handle = 0;
      }
    VkUnlock();
    for (int i = 0; i < kept_count; ++i) {
      struct {
        uint64_t device, swapchain;
        const void* allocator;
      } destroy = {kept_devices[i], kept_handles[i], NULL};
      VkLog("destroying the kept swapchain first", kept_handles[i], 0, 0);
      RealFor(handle)(handle, g_code_destroy, &destroy);
    }
    // The overlay copies into the images: they need TRANSFER_DST usage. On the PS5 a swapchain
    // of a size VideoOut does not scan out (1280x720) is images of Mesa's own, each present
    // blitted into a 1920x1080 framebuffer, and BeamNG's screen stayed black that way though
    // every frame held its menu (build 15). So the swapchain is made at the full size, the
    // framebuffers themselves, and the overlay scales the game's frame up before each present.
    // If the full size fails, the game's size is made as before.
    unsigned char info_copy[104];
    const unsigned char* game_info = p->info;
    for (int i = 0; i < 104; ++i) info_copy[i] = game_info[i];
    *(uint32_t*)(info_copy + 56) |= 0x2 /* VK_IMAGE_USAGE_TRANSFER_DST_BIT */;
    BOOL enlarged = OverlayCanScale(width, height, format);
    if (enlarged) {
      *(uint32_t*)(info_copy + 44) = g_full_width;
      *(uint32_t*)(info_copy + 48) = g_full_height;
      *(uint32_t*)(info_copy + 56) |= 0x1 /* VK_IMAGE_USAGE_TRANSFER_SRC_BIT */;
    }
    p->info = info_copy;
    long status = RealFor(handle)(handle, code, args);
    if (enlarged && (status || p->result)) {
      VkLog("create swapchain: the full size failed, status, result", (uint32_t)status, (uint32_t)p->result, 0);
      enlarged = 0;
      *(uint32_t*)(info_copy + 44) = width;
      *(uint32_t*)(info_copy + 48) = height;
      *(uint32_t*)(info_copy + 56) = usage | 0x2;
      p->result = 0;
      status = RealFor(handle)(handle, code, args);
    }
    p->info = game_info;
    uint32_t made_width = enlarged ? g_full_width : width, made_height = enlarged ? g_full_height : height;
    if (!status && p->result == 0 && p->swapchain)
      OverlayNoteSwapchain((void*)(uintptr_t)p->device, *p->swapchain, made_width, made_height, width, height,
                           format);
    VkLock();
    VkLog("create swapchain: old, size, result", old, ((uint64_t)width << 32) | height, (uint32_t)p->result);
    VkLog("  usage asked, format, size made", usage, format, ((uint64_t)made_width << 32) | made_height);
    if (!status && p->result == 0 && p->swapchain) {
      chain_t* previous = FindChain(g_current_chain);
      if (previous) previous->retired = 1;
      chain_t* old_chain = FindChain(old);
      if (old_chain) old_chain->retired = 1;
      for (int i = 0; i < kMaxChains; ++i)
        if (!g_chains[i].handle) {
          g_chains[i] = (chain_t){*p->swapchain, width, height, *(const uint32_t*)(p->info + 32), 0, 0,
                                  0, 0, surface, format, color_space, usage, present_mode, p->device, enlarged};
          break;
        }
      g_current_chain = *p->swapchain;
      VkLog("  new swapchain", *p->swapchain, 0, 0);
      LogChains("swapchains, current");
    }
    VkUnlock();
    return status;
  }
  if (code == g_code_destroy) {
    struct {
      uint64_t device, swapchain;
    }* p = args;
    VkLock();
    VkLog("destroy swapchain", p->swapchain, 0, 0);
    chain_t* chain = FindChain(p->swapchain);
    if (chain && g_vk_reuse && g_current_chain == p->swapchain) {
      // Kept for the identical create that usually follows; images the game held come back
      // through PresentOrphans.
      chain->parked = 1;
      chain->orphaned |= chain->acquired;
      chain->acquired = 0;
      VkLog("  kept it for reuse, held images", chain->orphaned, 0, 0);
      VkUnlock();
      return 0;
    }
    if (chain) {
      if (chain->acquired) VkLog("  it still had acquired images", chain->acquired, 0, 0);
      chain->handle = 0;
    }
    if (g_current_chain == p->swapchain) g_current_chain = 0;
    VkUnlock();
    OverlayForgetSwapchain(p->swapchain);
    return RealFor(handle)(handle, code, args);
  }
  if (code == g_code_get_queue) {
    struct {
      void* device;
      uint32_t family, index;
      void** queue;
    }* p = args;
    long status = RealFor(handle)(handle, code, args);
    if (!status && p->queue && *p->queue) OverlayNoteQueue(*p->queue, p->family);
    return status;
  }
  if (code == g_code_get_queue2) {
    struct {
      void* device;
      const unsigned char* info;  // VkDeviceQueueInfo2: queueFamilyIndex at 20
      void** queue;
    }* p = args;
    long status = RealFor(handle)(handle, code, args);
    if (!status && p->queue && *p->queue && p->info) OverlayNoteQueue(*p->queue, *(const uint32_t*)(p->info + 20));
    return status;
  }
  if (code == g_code_surface_caps || code == g_code_physical[0] || code == g_code_physical[1] ||
      code == g_code_physical[2]) {
    OverlayNotePhysical(*(void**)args);  // their first parameter is the physical device
    return RealFor(handle)(handle, code, args);
  }
  return RealFor(handle)(handle, code, args);
}

// Every call comes here: the thread that submits has its calls timed by kind (see g_vkt).
static long VkDispatcher(uint64_t handle, unsigned int code, void* args) {
  DWORD thread = GetCurrentThreadId();
  unsigned kind = code < kVkCodes ? g_call_kind[code] : kVkOther;
  if (thread != g_vkt.thread) {
    long calls = __sync_add_and_fetch(&g_vk_calls, 1);
    if (calls <= 40) VkLog("call: number, code, caller", calls, code, (uint64_t)(uintptr_t)__builtin_return_address(0));
    if (kind == kVkSubmit) NoteSubmitter(thread);
    return VkDispatch(handle, code, args);
  }
  // The timed thread counts its own calls, without a locked add.
  if (g_vkt.nested) kind = kVkOwn;
  ++g_vkt.calls[kind];
  if (kind == kVkSubmit) g_vkt.last_submit = GetTickCount64();
  if (kind == kVkOwn || !g_vkt.timing) return VkDispatch(handle, code, args);
  uint64_t start = Tsc();
  long status = VkDispatch(handle, code, args);
  int64_t spent = (int64_t)(Tsc() - start);
  if (spent > 0) g_vkt.ticks[kind] += (uint64_t)spent;  // not when a move to another core went back in time
  return status;
}

// The Unix call code a winevulkan thunk passes ("mov edx, imm32", or "mov ecx,
// imm32" for queue calls), and the dispatcher slot it calls through. 0 when
// the thunk does not look as expected.
static BOOL DecodeThunk(const unsigned char* code, BOOL queue_call, unsigned* call_code, unix_call_fn** slot) {
  BOOL have_code = 0;
  for (int i = 0; i < 0x60; ++i) {
    if (!have_code && code[i] == (queue_call ? 0xb9 : 0xba)) {
      *call_code = *(const uint32_t*)(code + i + 1);
      have_code = 1;
    }
    if (!queue_call && have_code && code[i] == 0xff && code[i + 1] == 0x15) {
      *slot = (unix_call_fn*)(code + i + 6 + *(const int32_t*)(code + i + 2));
      return 1;
    }
    if (queue_call && have_code) return 1;
  }
  return 0;
}

// Points one winevulkan.dll's swapchain calls at VkDispatcher. Every copy of the DLL a game
// loads has its own code and its own dispatcher pointer.
// Whether address is in module's image. Until a module first calls into its Unix side, its slot
// holds winecrt0's fallback, code of the module itself: that looks up ntdll's
// __wine_unix_call_dispatcher, writes it into the slot and jumps through it. So a call passed on to
// the fallback unhooks the module (build 17 hooked winevulkan.dll that early, and most calls went
// past the shim); ntdll's dispatcher, what the fallback would write, is called instead.
static BOOL InImage(HMODULE module, const void* address) {
  const unsigned char* base = (const unsigned char*)module;
  DWORD size = *(const DWORD*)(base + *(const int32_t*)(base + 0x3c) + 0x50);  // SizeOfImage
  return (const unsigned char*)address >= base && (const unsigned char*)address < base + size;
}

static unix_call_fn NtdllDispatcher(void) {
  static const WCHAR name[] = {'n', 't', 'd', 'l', 'l', '.', 'd', 'l', 'l', 0};
  HMODULE ntdll = GetModuleHandleW(name);
  unix_call_fn* variable = ntdll ? (unix_call_fn*)GetProcAddress(ntdll, "__wine_unix_call_dispatcher") : NULL;
  return variable ? *variable : NULL;
}

static void HookModule(HMODULE module) {
  const char* names[] = {"vkAcquireNextImageKHR", "vkAcquireNextImage2KHR", "vkCreateSwapchainKHR",
                         "vkDestroySwapchainKHR", "vkQueuePresentKHR"};
  // The codes of the winevulkan in Playden's kit 0.15; another build is left alone.
  static const unsigned expected[] = {0x4, 0x3, 0x191, 0x1c8, 0x2ab};
  unsigned codes[5];
  unix_call_fn* slot = NULL;
  for (int i = 0; i < 5; ++i) {
    const unsigned char* thunk = GetProcAddress(module, names[i]);
    unix_call_fn* this_slot = slot;
    if (!thunk || !DecodeThunk(thunk, i == 4, &codes[i], &this_slot) || (slot && this_slot != slot) ||
        codes[i] != expected[i]) {
      VkLog("a winevulkan.dll is not the expected build, left alone", (uint64_t)(uintptr_t)module, 0, 0);
      return;
    }
    slot = this_slot;
  }
  g_code_acquire = codes[0], g_code_acquire2 = codes[1], g_code_create = codes[2], g_code_destroy = codes[3];
  g_code_present = codes[4];
  // For the overlay: the calls that name a queue's family and the physical device.
  const char* extra_names[] = {"vkGetDeviceQueue",
                               "vkGetDeviceQueue2",
                               "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
                               "vkGetPhysicalDeviceMemoryProperties",
                               "vkGetPhysicalDeviceMemoryProperties2",
                               "vkGetPhysicalDeviceProperties"};
  static const unsigned extra_expected[] = {0x211, 0x212, 0x267, 0x251, 0x252, 0x257};
  unsigned* extra_codes[] = {&g_code_get_queue,  &g_code_get_queue2, &g_code_surface_caps,
                             &g_code_physical[0], &g_code_physical[1], &g_code_physical[2]};
  for (int i = 0; i < 6; ++i) {
    const unsigned char* thunk = GetProcAddress(module, extra_names[i]);
    unsigned extra_code = ~0u;
    unix_call_fn* extra_slot = slot;
    if (thunk && DecodeThunk(thunk, 0, &extra_code, &extra_slot) && extra_slot == slot && extra_code == extra_expected[i])
      *extra_codes[i] = extra_code;
    else
      VkLog("overlay: call not as expected, left alone", (uint64_t)i, extra_code, 0);
  }
  // For the second pace line: acquires and presents are timed where they are handled, and these
  // wait for the GPU, submit to it or poll its progress. The KHR aliases are not exported; in this
  // build each is the code after its core call's (read from the kit's winevulkan.dll).
  g_call_kind[codes[0]] = g_call_kind[codes[1]] = g_call_kind[codes[4]] = kVkOwn;
  const char* timing_names[] = {"vkWaitForFences", "vkWaitSemaphores", "vkQueueWaitIdle",
                                "vkDeviceWaitIdle", "vkQueueSubmit",    "vkQueueSubmit2",
                                "vkGetFenceStatus", "vkGetSemaphoreCounterValue"};
  static const unsigned timing_expected[] = {0x2e0, 0x2e3, 0x2b1, 0x1ce, 0x2ae, 0x2af, 0x218, 0x288};
  static const unsigned char timing_kind[] = {kVkWait,   kVkWait,   kVkWait, kVkWait,
                                              kVkSubmit, kVkSubmit, kVkPoll, kVkPoll};
  static const BOOL timing_alias[] = {0, 1, 0, 0, 0, 1, 0, 1};
  for (int i = 0; i < 8; ++i) {
    const unsigned char* thunk = GetProcAddress(module, timing_names[i]);
    unsigned timing_code = ~0u;
    unix_call_fn* timing_slot = slot;
    if (thunk && DecodeThunk(thunk, 0, &timing_code, &timing_slot) && timing_slot == slot &&
        timing_code == timing_expected[i]) {
      g_call_kind[timing_code] = timing_kind[i];
      if (timing_alias[i]) g_call_kind[timing_code + 1] = timing_kind[i];
    } else {
      VkLog("pace: call not as expected, counted with the others", (uint64_t)i, timing_code, 0);
    }
  }

  // The unixlib handle: "mov rcx, [rip + rel32]" in the acquire thunk.
  const uint64_t* handle_cell = NULL;
  const unsigned char* acquire = GetProcAddress(module, names[0]);
  for (int k = 0; k < 0x50; ++k)
    if (acquire[k] == 0x48 && acquire[k + 1] == 0x8b && acquire[k + 2] == 0x0d) {
      handle_cell = (const uint64_t*)(acquire + k + 7 + *(const int32_t*)(acquire + k + 3));
      break;
    }
  unix_call_fn real = *slot;
  if (InImage(module, (const void*)real)) {
    // winecrt0's fallback: see InImage.
    unix_call_fn resolved = NtdllDispatcher();
    VkLog("the slot holds winecrt0's fallback; calls go to ntdll's dispatcher", (uint64_t)(uintptr_t)real,
          (uint64_t)(uintptr_t)resolved, 0);
    if (resolved) real = resolved;
  }
  if (g_real_dispatcher && real != g_real_dispatcher && real != VkDispatcher)
    VkLog("this copy's dispatcher differs", (uint64_t)(uintptr_t)module, (uint64_t)(uintptr_t)real, 0);
  if (!g_real_dispatcher && real != VkDispatcher) g_real_dispatcher = real;
  VkLog("module, slot, dispatcher", (uint64_t)(uintptr_t)module, (uint64_t)(uintptr_t)slot, (uint64_t)(uintptr_t)real);

  // Each thunk calls through the slot: point its call straight at VkDispatcher ("call rel32"),
  // and the present thunk's helper too, whose calls through the slot are a call and a jump.
  int patched = 0;
  vk_module_t record = {module, handle_cell, (int32_t (*)(uint64_t, const void*))GetProcAddress(module, names[4]),
                        real == VkDispatcher ? g_real_dispatcher : real, slot, {0}, {{0}}, 0};
  for (int i = 0; i < 5; ++i) {
    unsigned char* thunk = GetProcAddress(module, names[i]);
    unsigned char* sites[4];
    int site_count = 0;
    unsigned char* scan[2] = {thunk, NULL};
    for (int k = 0; k < 0x60; ++k)
      if (i == 4 && thunk[k] == 0xe8) {  // the present thunk's call to its helper
        scan[1] = thunk + k + 5 + *(int32_t*)(thunk + k + 1);
        break;
      }
    for (int part = 0; part < 2; ++part) {
      unsigned char* code = scan[part];
      if (!code) continue;
      for (int k = 0; k < 0x70 && site_count < 4; ++k)
        if (code[k] == 0xff && (code[k + 1] == 0x15 || code[k + 1] == 0x25) &&
            code + k + 6 + *(int32_t*)(code + k + 2) == (unsigned char*)slot)
          sites[site_count++] = code + k;
    }
    for (int k = 0; k < site_count; ++k) {
      unsigned char* site = sites[k];
      int64_t distance = (int64_t)((uintptr_t)VkDispatcher - (uintptr_t)(site + 5));
      if (distance != (int32_t)distance) {
        VkLog("call site too far from the shim", (uint64_t)(uintptr_t)site, 0, 0);
        continue;
      }
      DWORD old_protect;
      if (!VirtualProtect(site, 6, 0x40 /* PAGE_EXECUTE_READWRITE */, &old_protect)) {
        VkLog("could not write call site", (uint64_t)(uintptr_t)site, 0, 0);
        continue;
      }
      unsigned char bytes[6] = {site[1] == 0x15 ? 0xe8 : 0xe9, 0, 0, 0, 0, 0x90};
      *(int32_t*)(bytes + 1) = (int32_t)distance;
      // A thread may run this code meanwhile: park it on a two-byte jump to itself, write the
      // rest, then put the new first two bytes in place.
      *(volatile uint16_t*)site = 0xfeeb;
      __sync_synchronize();
      for (int b = 2; b < 6; ++b) site[b] = bytes[b];
      __sync_synchronize();
      *(volatile uint16_t*)site = (uint16_t)(bytes[0] | bytes[1] << 8);
      VirtualProtect(site, 6, old_protect, &old_protect);
      FlushInstructionCache((HANDLE)(intptr_t)-1, site, 6);
      ++patched;
      if (record.site_count < 16) {
        record.sites[record.site_count] = site;
        for (int b = 0; b < 6; ++b) record.site_bytes[record.site_count][b] = bytes[b];
        ++record.site_count;
      }
    }
  }

  DWORD old_protect;
  if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protect)) {
    __sync_synchronize();
    *slot = VkDispatcher;
    VirtualProtect(slot, sizeof(*slot), old_protect, &old_protect);
  }
  VkLog("call sites patched, slot now", patched, (uint64_t)(uintptr_t)*(void* volatile*)slot, 0);
  if (g_vk_module_count < 4) g_vk_modules[g_vk_module_count++] = record;
  if (!g_queue_present) g_queue_present = g_vk_modules[0].present;
}

static BOOL SameNameNoCase(const WCHAR* a, const char* b) {
  for (; *a && *b; ++a, ++b) {
    WCHAR x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
    if (x != (unsigned char)*b) return 0;
  }
  return !*a && !*b;
}

// Puts a patch back if something restored the original bytes, and says so.
static void VerifyPatches(void) {
  for (int m = 0; m < g_vk_module_count; ++m) {
    vk_module_t* record = &g_vk_modules[m];
    unix_call_fn current = (unix_call_fn)*(void* volatile*)record->slot;
    if (current != VkDispatcher) {
      // What the module put there is the dispatcher to call from now on (see InImage): passed on to
      // winecrt0's fallback, every call would put ntdll's dispatcher back in the slot.
      static long restores;
      if (!InImage(record->module, (const void*)current) && current != record->real) {
        VkLog("the module set its dispatcher; calls go to it now", (uint64_t)(uintptr_t)record->real,
              (uint64_t)(uintptr_t)current, 0);
        if (g_real_dispatcher == record->real) g_real_dispatcher = current;
        record->real = current;
        __sync_synchronize();
      } else if (++restores <= 20 || restores % 100 == 0) {
        VkLog("dispatcher slot was restored, setting it again: slot, value, times", (uint64_t)(uintptr_t)record->slot,
              (uint64_t)(uintptr_t)current, (uint64_t)restores);
      }
      DWORD old_protect;
      if (VirtualProtect(record->slot, 8, PAGE_READWRITE, &old_protect)) {
        *record->slot = VkDispatcher;
        VirtualProtect(record->slot, 8, old_protect, &old_protect);
      }
    }
    for (int k = 0; k < record->site_count; ++k) {
      unsigned char* site = record->sites[k];
      BOOL same = 1;
      for (int b = 0; b < 6; ++b) same &= ((volatile unsigned char*)site)[b] == record->site_bytes[k][b];
      if (same) continue;
      VkLog("call site was restored, patching it again: site, bytes", (uint64_t)(uintptr_t)site,
            *(volatile uint64_t*)site, 0);
      DWORD old_protect;
      if (VirtualProtect(site, 6, 0x40, &old_protect)) {
        *(volatile uint16_t*)site = 0xfeeb;
        __sync_synchronize();
        for (int b = 2; b < 6; ++b) site[b] = record->site_bytes[k][b];
        __sync_synchronize();
        *(volatile uint16_t*)site = (uint16_t)(record->site_bytes[k][0] | record->site_bytes[k][1] << 8);
        VirtualProtect(site, 6, old_protect, &old_protect);
        FlushInstructionCache((HANDLE)(intptr_t)-1, site, 6);
      }
    }
  }
}

// Watches for winevulkan.dll and hooks every copy that loads; keeps checking the patches.
static DWORD WINAPI HookVulkanLater(void* unused) {
  (void)unused;
  static unsigned char entry[1080];  // MODULEENTRY32W
  MeasureClocks();
  for (int round = 0;; ++round) {
    if (g_vk_module_count) VerifyPatches();
    if (round % 100 == 0 && g_vk_module_count)
      VkLog("calls through the shim so far",
            (uint64_t)(g_vk_calls + g_vkt.calls[0] + g_vkt.calls[1] + g_vkt.calls[2] + g_vkt.calls[3] + g_vkt.calls[4]),
            0, 0);
    if (round % 100 == 50) {
      VkLog("cef frames so far: painted, of them empty, accelerated", g_paints, g_blank_paints, g_accelerated_paints);
      static long threads_logged;
      if (g_cef_threads != threads_logged)
        VkLog("cef threads made with the bigger stack", threads_logged = g_cef_threads, g_cef_stack >> 10, 0);
    }
    if (round % 300 == 299) CopyGameLog();
    // Until winevulkan.dll is found, for three minutes at most: a module list every 100 ms is not free.
    if (round >= 1800 || g_vk_module_count) {
      Sleep(100);
      continue;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(8 /* TH32CS_SNAPMODULE */, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
      *(DWORD*)entry = sizeof(entry);
      for (BOOL more = Module32FirstW(snapshot, entry); more; *(DWORD*)entry = sizeof(entry),
                more = Module32NextW(snapshot, entry)) {
        HMODULE module = *(HMODULE*)(entry + 40);
        if (!SameNameNoCase((const WCHAR*)(entry + 48), "winevulkan.dll")) continue;
        BOOL known = 0;
        for (int i = 0; i < g_vk_module_count; ++i) known |= g_vk_modules[i].module == module;
        if (known || g_vk_module_count >= 4) continue;
        VkLog("found a winevulkan.dll, after milliseconds", (uint64_t)(uintptr_t)module, (uint64_t)round * 100, 0);
        const WCHAR* path = (const WCHAR*)(entry + 560);
        size_t length = 0;
        while (length < 259 && path[length]) ++length;
        LogWide("libcef-shim: vk   path ", path, length);
        HookModule(module);
        if (g_vk_module_count == 1)
          Log(g_vk_fix ? "libcef-shim: vk: swapchain watch on, with the fix\n"
                       : "libcef-shim: vk: swapchain watch on, log only\n");
      }
      CloseHandle(snapshot);
    }
    Sleep(100);
  }
  return 0;
}
