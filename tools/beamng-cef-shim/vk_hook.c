// SPDX-License-Identifier: GPL-3.0-or-later
//
// Swapchain watch for the PS5 display, included by shim.c.
//
// Every winevulkan call goes through one pointer, the Unix call dispatcher.
// The shim puts its own function there, logs swapchain calls (create, destroy,
// acquire, present) and passes every call on unchanged, with one exception:
// on the PS5, swapchains of one size share their display buffers, and a
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
static int32_t (*g_queue_present)(uint64_t queue, const void* info);

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
  VkUnlock();
  if (!count || !queue || !g_queue_present) return 0;
  for (uint32_t i = 0; i < count; ++i) {
    present_info_t info = {1000001001 /* VK_STRUCTURE_TYPE_PRESENT_INFO_KHR */, NULL, 0, NULL, 1, &chains[i],
                           &indices[i], NULL};
    int32_t result = g_queue_present(queue, &info);
    VkLog("released stuck image: chain, index, result", chains[i], indices[i], (uint32_t)result);
    VkLock();
    chain_t* chain = FindChain(chains[i]);
    if (chain) chain->acquired &= ~(1u << indices[i]);
    VkUnlock();
  }
  return 1;
}

static void NotePresent(uint64_t queue, const present_info_t* info, int32_t result) {
  VkLock();
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
    long status = g_real_dispatcher(handle, code, args);
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

static long VkDispatcher(uint64_t handle, unsigned int code, void* args) {
  if (code == g_code_acquire) {
    struct {
      uint64_t device, swapchain, timeout, semaphore, fence;
      uint32_t* index;
      int32_t result;
    }* p = args;
    long status = Acquire(handle, code, args, p->swapchain, &p->timeout, &p->result);
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
    long status = Acquire(handle, code, args, p->info->swapchain, &p->info->timeout, &p->result);
    if (!status) NoteAcquire(p->info->swapchain, p->index ? *p->index : 0, p->result);
    return status;
  }
  if (code == g_code_present) {
    struct {
      uint64_t queue;
      const present_info_t* info;
      int32_t result;
    }* p = args;
    long status = g_real_dispatcher(handle, code, args);
    NotePresent(p->queue, p->info, status ? -1 : p->result);
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
    long status = g_real_dispatcher(handle, code, args);
    uint32_t width = *(const uint32_t*)(p->info + 44), height = *(const uint32_t*)(p->info + 48);
    uint64_t old = *(const uint64_t*)(p->info + 96);
    VkLock();
    VkLog("create swapchain: old, size, result", old, ((uint64_t)width << 32) | height, (uint32_t)p->result);
    if (!status && p->result == 0 && p->swapchain) {
      chain_t* previous = FindChain(g_current_chain);
      if (previous) previous->retired = 1;
      chain_t* old_chain = FindChain(old);
      if (old_chain) old_chain->retired = 1;
      for (int i = 0; i < kMaxChains; ++i)
        if (!g_chains[i].handle) {
          g_chains[i] = (chain_t){*p->swapchain, width, height, *(const uint32_t*)(p->info + 32), 0, 0};
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
    if (chain) {
      if (chain->acquired) VkLog("  it still had acquired images", chain->acquired, 0, 0);
      chain->handle = 0;
    }
    if (g_current_chain == p->swapchain) g_current_chain = 0;
    VkUnlock();
    return g_real_dispatcher(handle, code, args);
  }
  return g_real_dispatcher(handle, code, args);
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

static DWORD WINAPI HookVulkanLater(void* unused) {
  (void)unused;
  static const WCHAR name[] = {'w', 'i', 'n', 'e', 'v', 'u', 'l', 'k', 'a', 'n', '.', 'd', 'l', 'l', 0};
  HMODULE module = NULL;
  for (int i = 0; i < 6000 && !module; ++i) {  // up to two minutes
    module = GetModuleHandleW(name);
    if (!module) Sleep(20);
  }
  if (!module) {
    Log("libcef-shim: vk: winevulkan.dll never loaded\n");
    return 0;
  }
  const char* names[] = {"vkAcquireNextImageKHR", "vkAcquireNextImage2KHR", "vkCreateSwapchainKHR",
                         "vkDestroySwapchainKHR", "vkQueuePresentKHR"};
  unsigned* codes[] = {&g_code_acquire, &g_code_acquire2, &g_code_create, &g_code_destroy, &g_code_present};
  // The codes of the winevulkan in Playden's kit 0.15; another build is left alone.
  static const unsigned expected[] = {0x4, 0x3, 0x191, 0x1c8, 0x2ab};
  unix_call_fn* slot = NULL;
  for (int i = 0; i < 5; ++i) {
    const unsigned char* thunk = GetProcAddress(module, names[i]);
    unix_call_fn* this_slot = slot;
    if (!thunk || !DecodeThunk(thunk, i == 4, codes[i], &this_slot) || (slot && this_slot != slot) ||
        *codes[i] != expected[i]) {
      Log("libcef-shim: vk: winevulkan.dll is not the expected build, swapchain watch off\n");
      return 0;
    }
    slot = this_slot;
    VkLog(names[i], *codes[i], 0, 0);
  }
  g_queue_present = (int32_t (*)(uint64_t, const void*))GetProcAddress(module, "vkQueuePresentKHR");
  DWORD old_protect;
  if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old_protect)) {
    Log("libcef-shim: vk: could not write the dispatcher slot\n");
    return 0;
  }
  g_dispatcher_slot = slot;
  g_real_dispatcher = *slot;
  __sync_synchronize();
  *slot = VkDispatcher;
  VirtualProtect(slot, sizeof(*slot), old_protect, &old_protect);
  Log(g_vk_fix ? "libcef-shim: vk: swapchain watch on, with the fix\n" : "libcef-shim: vk: swapchain watch on, log only\n");
  return 0;
}
