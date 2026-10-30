// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in swapchains for the game's second windows, included by shim.c after overlay.c.
//
// On the PS5 every window shows on the one display, and Wine refuses a swapchain for a second
// window while the game's main one is there (VK_ERROR_NATIVE_WINDOW_IN_USE_KHR). Some of BeamNG's
// debug pop-ups open a window of their own (the Performance Graph's Export): BeamNG asked ten
// times, then used the swapchain it did not get and crashed. Such a window now gets a stand-in:
// images of the shim's own, which the game draws into and presents, and which nothing shows.
// - vkGetSwapchainImagesKHR gives the game those images;
// - vkAcquireNextImageKHR(2) hands out the next one at once. The semaphore and fence it should
//   signal are signalled by an empty submit just before the game's next submit to any queue (the
//   first that can wait on them), on that queue, which the game holds for its submit;
// - vkQueuePresentKHR passes the other swapchains on and drops the stand-ins; when only stand-ins
//   are left, an empty submit on the queue waits on the present's semaphores instead;
// - vkDestroySwapchainKHR frees the images. Debug names and HDR metadata for a stand-in are taken
//   and dropped: Wine would read a stand-in as one of its own swapchains.

enum { kStandIns = 8, kStandInImages = 4, kWindowInUse = -1000000001, kOwed = 16, kPresentMax = 16 };

typedef struct {
  BOOL used;
  vk_dispatch_t device;
  uint32_t width, height, image_count, next;
  vk_handle_t images[kStandInImages], memories[kStandInImages];
} standin_t;

static standin_t g_standins[kStandIns];
static vk_handle_t g_owed_semaphores[kOwed], g_owed_fences[kOwed];
static int g_owed_semaphore_count, g_owed_fence_count;
static volatile long g_owed;  // anything owed; read without the lock

// The calls that name a swapchain, in kit 0.15's winevulkan.dll (vk_hook.c checks it is that build
// and reads the codes of the exported ones). The debug name calls and vkSetHdrMetadataEXT are not
// exported; their codes were read from that build's function table.
static const unsigned kCodeNames[4] = {0x2c0 /* vkSetDebugUtilsObjectNameEXT */,
                                       0x2c1 /* vkSetDebugUtilsObjectTagEXT */,
                                       0x198 /* vkDebugMarkerSetObjectNameEXT */,
                                       0x199 /* vkDebugMarkerSetObjectTagEXT */};
static const unsigned kCodeHdr = 0x2c5;  // vkSetHdrMetadataEXT

static standin_t* StandIn(uint64_t handle) {
  uint64_t first = (uint64_t)(uintptr_t)&g_standins[0], end = (uint64_t)(uintptr_t)&g_standins[kStandIns];
  if (handle < first || handle >= end || (handle - first) % sizeof(standin_t)) return NULL;
  standin_t* standin = (standin_t*)(uintptr_t)handle;
  return standin->used ? standin : NULL;
}

static BOOL IsStandIn(uint64_t handle) { return StandIn(handle) != NULL; }

static void StandInFree(standin_t* standin) {
  for (uint32_t i = 0; i < kStandInImages; ++i) {
    if (standin->images[i]) g_vkf.destroy_image(standin->device, standin->images[i], NULL);
    if (standin->memories[i]) g_vkf.free_memory(standin->device, standin->memories[i], NULL);
    standin->images[i] = standin->memories[i] = 0;
  }
  standin->image_count = 0;
}

// A stand-in for the swapchain create info asks for: its size, format, layers, usage and sharing,
// minImageCount images (two to four). 0 when it could not be made.
static uint64_t StandInCreate(vk_dispatch_t device, const unsigned char* info) {
  if (!g_vkf.create_image && !(g_vk_module_count && OverlayLoadFunctions(g_vk_modules[0].module))) return 0;
  if (!g_ov.physical) return 0;  // no memory types to pick from
  standin_t* standin = NULL;
  VkLock();
  for (int i = 0; i < kStandIns && !standin; ++i)
    if (!g_standins[i].used) {
      standin = &g_standins[i];
      standin->used = 1;
    }
  VkUnlock();
  if (!standin) return 0;
  uint32_t count = *(const uint32_t*)(info + 32), layers = *(const uint32_t*)(info + 52);
  count = count < 2 ? 2 : count > kStandInImages ? kStandInImages : count;
  standin->device = device;
  standin->width = *(const uint32_t*)(info + 44);
  standin->height = *(const uint32_t*)(info + 48);
  standin->next = 0;
  vk_image_create_info_t image_info = {14 /* IMAGE_CREATE_INFO */,
                                       NULL,
                                       (*(const uint32_t*)(info + 16) & 0x4) ? 0x8u : 0u,  // mutable format
                                       1 /* 2D */,
                                       *(const uint32_t*)(info + 36),
                                       standin->width,
                                       standin->height,
                                       1,
                                       1,
                                       layers ? layers : 1,
                                       1,
                                       0 /* OPTIMAL */,
                                       *(const uint32_t*)(info + 56),
                                       *(const uint32_t*)(info + 60),
                                       *(const uint32_t*)(info + 64),
                                       *(const uint32_t* const*)(info + 72),
                                       kOvLayoutUndefined};
  for (uint32_t i = 0; i < count; ++i) {
    vk_handle_t image = 0, memory = 0;
    int32_t result = g_vkf.create_image(device, &image_info, NULL, &image);
    if (!result) {
      standin->images[i] = image;
      vk_memory_requirements_t requirements;
      g_vkf.get_image_memory_requirements(device, image, &requirements);
      uint32_t type = OverlayMemoryType(requirements.memory_type_bits, 0x1 /* DEVICE_LOCAL */);
      if (type == ~0u) type = OverlayMemoryType(requirements.memory_type_bits, 0);
      vk_memory_allocate_info_t memory_info = {5 /* MEMORY_ALLOCATE_INFO */, NULL, requirements.size, type};
      result = type == ~0u ? -2 : g_vkf.allocate_memory(device, &memory_info, NULL, &memory);
      if (!result) {
        standin->memories[i] = memory;
        result = g_vkf.bind_image_memory(device, image, memory, 0);
      }
    }
    if (result) {
      VkLog("stand-in swapchain: an image failed, result", (uint32_t)result, i, 0);
      StandInFree(standin);
      standin->used = 0;
      return 0;
    }
    standin->image_count = i + 1;
  }
  return (uint64_t)(uintptr_t)standin;
}

static BOOL StandInDestroy(uint64_t handle) {
  standin_t* standin = StandIn(handle);
  if (!standin) return 0;
  StandInFree(standin);
  VkLog("stand-in swapchain destroyed", handle, 0, 0);
  standin->used = 0;
  return 1;
}

// vkGetSwapchainImagesKHR for a stand-in.
static BOOL StandInImages(void* args) {
  struct {
    vk_dispatch_t device;
    uint64_t swapchain;
    uint32_t* count;
    vk_handle_t* images;
    int32_t result;
  }* p = args;
  standin_t* standin = StandIn(p->swapchain);
  if (!standin) return 0;
  if (!p->images) {
    *p->count = standin->image_count;
    p->result = 0;
    return 1;
  }
  uint32_t count = *p->count < standin->image_count ? *p->count : standin->image_count;
  for (uint32_t i = 0; i < count; ++i) p->images[i] = standin->images[i];
  *p->count = count;
  p->result = count < standin->image_count ? 5 /* VK_INCOMPLETE */ : 0;
  return 1;
}

// vkAcquireNextImageKHR(2) for a stand-in: the next image, at once.
static BOOL StandInAcquire(uint64_t swapchain, uint64_t semaphore, uint64_t fence, uint32_t* index,
                           int32_t* result) {
  standin_t* standin = StandIn(swapchain);
  if (!standin) return 0;
  VkLock();
  if (index) *index = standin->next;
  standin->next = (standin->next + 1) % standin->image_count;
  if (semaphore && g_owed_semaphore_count < kOwed) g_owed_semaphores[g_owed_semaphore_count++] = semaphore;
  if (fence && g_owed_fence_count < kOwed) g_owed_fences[g_owed_fence_count++] = fence;
  g_owed = g_owed_semaphore_count || g_owed_fence_count;
  VkUnlock();
  *result = 0;
  return 1;
}

// Before a submit of the game's, on its queue: signals what acquires of stand-ins owe.
static void StandInPayOwed(vk_dispatch_t queue) {
  if (!g_owed || !queue) return;
  vk_handle_t semaphores[kOwed], fences[kOwed];
  int semaphore_count, fence_count;
  VkLock();
  semaphore_count = g_owed_semaphore_count;
  fence_count = g_owed_fence_count;
  for (int i = 0; i < semaphore_count; ++i) semaphores[i] = g_owed_semaphores[i];
  for (int i = 0; i < fence_count; ++i) fences[i] = g_owed_fences[i];
  g_owed_semaphore_count = g_owed_fence_count = 0;
  g_owed = 0;
  VkUnlock();
  vk_submit_info_t submit = {4 /* SUBMIT_INFO */, NULL, 0, NULL, NULL, 0, NULL, (uint32_t)semaphore_count,
                             semaphores};
  int32_t result = g_vkf.queue_submit(queue, 1, &submit, fence_count ? fences[0] : 0);
  for (int i = 1; i < fence_count && !result; ++i) result = g_vkf.queue_submit(queue, 0, NULL, fences[i]);
  static long logged;
  if (result && ++logged <= 4)
    VkLog("stand-in swapchain: signalling an acquire failed, result", (uint32_t)result, 0, 0);
}

// For a present: the swapchains that are not stand-ins, into without (its arrays given), and how
// many there are; -1 when there is no stand-in among them.
static int StandInFilter(const present_info_t* asked, present_info_t* without, uint64_t* chains, uint32_t* indices,
                         int32_t* results) {
  if (!asked || !asked->swapchain_count || asked->swapchain_count > kPresentMax) return -1;
  BOOL any = 0;
  for (uint32_t i = 0; i < asked->swapchain_count && !any; ++i) any = StandIn(asked->swapchains[i]) != NULL;
  if (!any) return -1;
  *without = *asked;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < asked->swapchain_count; ++i)
    if (!StandIn(asked->swapchains[i])) {
      chains[kept] = asked->swapchains[i];
      indices[kept++] = asked->indices[i];
    }
  without->swapchain_count = kept;
  without->swapchains = chains;
  without->indices = indices;
  without->results = asked->results ? results : NULL;
  return (int)kept;
}

// After the present without the stand-ins: each swapchain's result where the game asked for them.
static void StandInResults(const present_info_t* asked, const present_info_t* without) {
  if (!asked->results) return;
  uint32_t kept = 0;
  for (uint32_t i = 0; i < asked->swapchain_count; ++i)
    asked->results[i] = StandIn(asked->swapchains[i]) ? 0 : without->results[kept++];
}

// A present of stand-ins only: its semaphores are waited on by an empty submit, as a present would.
static void StandInConsume(vk_dispatch_t queue, const present_info_t* asked) {
  uint32_t stages[kPresentMax];
  for (uint32_t done = 0; done < asked->wait_count;) {
    uint32_t count = asked->wait_count - done < kPresentMax ? asked->wait_count - done : kPresentMax;
    for (uint32_t i = 0; i < count; ++i) stages[i] = 0x10000;  // ALL_COMMANDS
    vk_submit_info_t submit = {4, NULL, count, (const vk_handle_t*)asked->waits + done, stages, 0, NULL, 0, NULL};
    int32_t result = g_vkf.queue_submit(queue, 1, &submit, 0);
    static long logged;
    if (result && ++logged <= 4) VkLog("stand-in swapchain: a present's wait failed, result", (uint32_t)result, 0, 0);
    done += count;
  }
  if (asked->results)
    for (uint32_t i = 0; i < asked->swapchain_count; ++i) asked->results[i] = 0;
}

// The calls with a swapchain or an acquire's semaphore that are not handled where vk_hook.c
// handles create, destroy, acquire and present. 1 when the call was answered here.
static BOOL StandInCall(uint64_t handle, unsigned code, void* args) {
  if (code == g_code_get_images) return StandInImages(args);
  if (code == g_code_submit || code == g_code_submit2 || code == g_code_submit2 + 1) {
    StandInPayOwed(*(vk_dispatch_t*)args);  // the queue comes first in each
    return 0;
  }
  for (int i = 0; i < 4; ++i)
    if (code == kCodeNames[i]) {
      struct {
        vk_dispatch_t device;
        const unsigned char* info;  // the object's handle at 24 in each of the four
        int32_t result;
      }* p = args;
      if (!p->info || !StandIn(*(const uint64_t*)(p->info + 24))) return 0;
      p->result = 0;
      return 1;
    }
  if (code == kCodeHdr) {
    struct {
      vk_dispatch_t device;
      uint32_t count;
      const uint64_t* swapchains;
      const unsigned char* metadata;  // VkHdrMetadataEXT, 64 bytes each
    }* p = args;
    if (!p->swapchains || !p->count || p->count > kPresentMax) return 0;
    uint64_t chains[kPresentMax];
    static unsigned char metadata[kPresentMax * 64];
    uint32_t kept = 0;
    for (uint32_t i = 0; i < p->count; ++i)
      if (!StandIn(p->swapchains[i])) {
        chains[kept] = p->swapchains[i];
        for (int k = 0; k < 64; ++k) metadata[kept * 64 + k] = p->metadata[i * 64 + k];
        ++kept;
      }
    if (kept == p->count) return 0;
    if (!kept) return 1;
    uint32_t count = p->count;
    const uint64_t* swapchains = p->swapchains;
    const unsigned char* given = p->metadata;
    p->count = kept;
    p->swapchains = chains;
    p->metadata = metadata;
    RealFor(handle)(handle, code, args);
    p->count = count;
    p->swapchains = swapchains;
    p->metadata = given;
    return 1;
  }
  return 0;
}
