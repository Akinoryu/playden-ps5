// SPDX-License-Identifier: GPL-3.0-or-later
//
// Menu overlay, included by shim.c after vk_hook.c and paint_hook.c.
//
// On the console CEF draws BeamNG's menus correctly (frames saved by
// paint_hook.c), BeamNG gets every frame through OnPaint and presents at 60 fps,
// yet the screen stays black: BeamNG's own drawing of its UI texture does not
// reach the swapchain. So while CEF's latest frame is opaque (loading screens and
// menus cover the whole screen), the shim copies it into each swapchain image
// just before the game presents it: one buffer-to-image copy on the presenting
// queue, after the game's rendering (its present semaphores) and before the
// present (a semaphore of the shim's). While the UI is see-through (a HUD over
// the game) the game's own frame is shown. shim:no-overlay turns it off.
//
// The Vulkan calls go through winevulkan.dll's exports, with the game's device
// and queue. The structures below are the few this needs, with the layout of
// vulkan_core.h on x86-64.

typedef uint64_t vk_handle_t;  // non-dispatchable handles
typedef void* vk_dispatch_t;   // VkDevice, VkQueue, VkCommandBuffer, VkPhysicalDevice

typedef struct {
  uint32_t s_type;
  const void* next;
  uint32_t flags;
  uint32_t queue_family_index;
} vk_command_pool_create_info_t;

typedef struct {
  uint32_t s_type;
  const void* next;
  vk_handle_t command_pool;
  uint32_t level;
  uint32_t command_buffer_count;
} vk_command_buffer_allocate_info_t;

typedef struct {
  uint32_t s_type;
  const void* next;
  uint32_t flags;
  const void* inheritance_info;
} vk_command_buffer_begin_info_t;

typedef struct {
  uint32_t s_type;
  const void* next;
  uint32_t src_access_mask, dst_access_mask;
  uint32_t old_layout, new_layout;
  uint32_t src_queue_family_index, dst_queue_family_index;
  vk_handle_t image;
  uint32_t aspect_mask, base_mip_level, level_count, base_array_layer, layer_count;
} vk_image_memory_barrier_t;

typedef struct {
  uint64_t buffer_offset;
  uint32_t buffer_row_length, buffer_image_height;
  uint32_t aspect_mask, mip_level, base_array_layer, layer_count;
  int32_t x, y, z;
  uint32_t width, height, depth;
} vk_buffer_image_copy_t;

typedef struct {
  uint32_t s_type;
  const void* next;
  uint32_t wait_semaphore_count;
  const vk_handle_t* wait_semaphores;
  const uint32_t* wait_dst_stage_mask;
  uint32_t command_buffer_count;
  const vk_dispatch_t* command_buffers;
  uint32_t signal_semaphore_count;
  const vk_handle_t* signal_semaphores;
} vk_submit_info_t;

typedef struct {
  uint32_t s_type;
  const void* next;
  uint32_t flags;
} vk_simple_create_info_t;  // VkFenceCreateInfo, VkSemaphoreCreateInfo

typedef struct {
  uint32_t s_type;
  const void* next;
  uint32_t flags;
  uint64_t size;
  uint32_t usage;
  uint32_t sharing_mode;
  uint32_t queue_family_index_count;
  const uint32_t* queue_family_indices;
} vk_buffer_create_info_t;

typedef struct {
  uint64_t size, alignment;
  uint32_t memory_type_bits;
} vk_memory_requirements_t;

typedef struct {
  uint32_t s_type;
  const void* next;
  uint64_t allocation_size;
  uint32_t memory_type_index;
} vk_memory_allocate_info_t;

typedef struct {
  uint32_t memory_type_count;
  struct {
    uint32_t property_flags, heap_index;
  } memory_types[32];
  uint32_t memory_heap_count;
  struct {
    uint64_t size;
    uint32_t flags;
  } memory_heaps[16];
} vk_physical_device_memory_properties_t;

enum {
  kOvMaxImages = 8,
  kOvLayoutUndefined = 0,
  kOvLayoutTransferDst = 7,
  kOvLayoutPresent = 1000001002,
  kOvStageTransfer = 0x1000,
  kOvStageBottom = 0x2000,
  kOvAccessTransferWrite = 0x1000,
  kOvFormatB8G8R8A8Unorm = 44,
  kOvFormatB8G8R8A8Srgb = 50,
  kOvMaxWidth = 1920,  // the largest UI frame the overlay takes
  kOvMaxHeight = 1080,
};

// winevulkan.dll's entry points.
static struct {
  int32_t (*create_command_pool)(vk_dispatch_t, const void*, const void*, vk_handle_t*);
  int32_t (*allocate_command_buffers)(vk_dispatch_t, const void*, vk_dispatch_t*);
  int32_t (*begin_command_buffer)(vk_dispatch_t, const void*);
  int32_t (*end_command_buffer)(vk_dispatch_t);
  int32_t (*reset_command_buffer)(vk_dispatch_t, uint32_t);
  void (*cmd_pipeline_barrier)(vk_dispatch_t, uint32_t, uint32_t, uint32_t, uint32_t, const void*, uint32_t,
                               const void*, uint32_t, const void*);
  void (*cmd_copy_buffer_to_image)(vk_dispatch_t, vk_handle_t, vk_handle_t, uint32_t, uint32_t, const void*);
  int32_t (*queue_submit)(vk_dispatch_t, uint32_t, const void*, vk_handle_t);
  int32_t (*create_fence)(vk_dispatch_t, const void*, const void*, vk_handle_t*);
  int32_t (*wait_for_fences)(vk_dispatch_t, uint32_t, const vk_handle_t*, uint32_t, uint64_t);
  int32_t (*reset_fences)(vk_dispatch_t, uint32_t, const vk_handle_t*);
  int32_t (*create_semaphore)(vk_dispatch_t, const void*, const void*, vk_handle_t*);
  int32_t (*create_buffer)(vk_dispatch_t, const void*, const void*, vk_handle_t*);
  void (*get_buffer_memory_requirements)(vk_dispatch_t, vk_handle_t, void*);
  int32_t (*allocate_memory)(vk_dispatch_t, const void*, const void*, vk_handle_t*);
  int32_t (*bind_buffer_memory)(vk_dispatch_t, vk_handle_t, vk_handle_t, uint64_t);
  int32_t (*map_memory)(vk_dispatch_t, vk_handle_t, uint64_t, uint64_t, uint32_t, void**);
  void (*get_physical_device_memory_properties)(vk_dispatch_t, void*);
  int32_t (*get_swapchain_images)(vk_dispatch_t, vk_handle_t, uint32_t*, vk_handle_t*);
} g_vkf;

static struct {
  BOOL failed;            // something did not work: the overlay is off for good
  vk_dispatch_t physical;  // from vkGetPhysicalDeviceSurfaceCapabilitiesKHR
  vk_dispatch_t device;    // the swapchain's
  struct {
    vk_dispatch_t queue;
    uint32_t family;
  } queues[16];
  // The swapchain the images belong to.
  vk_handle_t swapchain;
  uint32_t width, height, format, image_count;
  vk_handle_t images[kOvMaxImages];
  // Per image: its command buffer, the fence of its last copy, the semaphore the present waits on.
  vk_dispatch_t command_buffers[kOvMaxImages];
  vk_handle_t fences[kOvMaxImages], semaphores[kOvMaxImages];
  BOOL ready;
  vk_dispatch_t ready_queue;
  vk_handle_t pool;
  vk_handle_t staging, staging_memory;
  void* staging_map;
  uint32_t staging_width, staging_height;
  long overlaid;
} g_ov;

static BOOL g_no_overlay;

// CEF's latest frame, copied by OnPaint; the staging buffer takes it at the next present.
static volatile long g_frame_lock;
static unsigned char* g_frame;
static uint32_t g_frame_width, g_frame_height;
static BOOL g_frame_dirty, g_frame_opaque;

static void FrameLock(void) {
  while (__sync_lock_test_and_set(&g_frame_lock, 1)) Sleep(0);
}
static void FrameUnlock(void) { __sync_lock_release(&g_frame_lock); }

// Called from OnPaintHook with each full-view frame.
static void OverlayTakeFrame(const void* pixels, int width, int height, int opaque_samples) {
  if (g_no_overlay || width <= 0 || height <= 0 || width > kOvMaxWidth || height > kOvMaxHeight) return;
  FrameLock();
  if (!g_frame) g_frame = VirtualAlloc(NULL, (size_t)kOvMaxWidth * kOvMaxHeight * 4, 0x3000, PAGE_READWRITE);
  if (g_frame) {
    const unsigned char* from = pixels;
    size_t bytes = (size_t)width * (size_t)height * 4;
    for (size_t i = 0; i < bytes; ++i) g_frame[i] = from[i];
    g_frame_width = (uint32_t)width;
    g_frame_height = (uint32_t)height;
    g_frame_dirty = 1;
    g_frame_opaque = opaque_samples >= 135;  // of 144: a loading screen or menu, not a HUD
  }
  FrameUnlock();
}

static void OverlayFail(const char* what, int32_t result) {
  g_ov.failed = 1;
  Log("libcef-shim: overlay off, this step failed: ");
  Log(what);
  Log("\n");
  VkLog("  result", (uint32_t)result, 0, 0);
}

static BOOL OverlayLoadFunctions(HMODULE module) {
  if (g_vkf.queue_submit) return 1;
  const char* names[] = {"vkCreateCommandPool", "vkAllocateCommandBuffers", "vkBeginCommandBuffer",
                         "vkEndCommandBuffer", "vkResetCommandBuffer", "vkCmdPipelineBarrier",
                         "vkCmdCopyBufferToImage", "vkQueueSubmit", "vkCreateFence", "vkWaitForFences",
                         "vkResetFences", "vkCreateSemaphore", "vkCreateBuffer", "vkGetBufferMemoryRequirements",
                         "vkAllocateMemory", "vkBindBufferMemory", "vkMapMemory",
                         "vkGetPhysicalDeviceMemoryProperties", "vkGetSwapchainImagesKHR"};
  void** slots = (void**)&g_vkf;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    slots[i] = GetProcAddress(module, names[i]);
    if (!slots[i]) {
      OverlayFail(names[i], 0);
      return 0;
    }
  }
  return 1;
}

static uint32_t QueueFamily(vk_dispatch_t queue) {
  for (int i = 0; i < 16; ++i)
    if (g_ov.queues[i].queue == queue) return g_ov.queues[i].family;
  return 0;
}

// From the dispatcher: vkGetDeviceQueue(2) results and the physical device the game asks about.
static void OverlayNotePhysical(void* physical) {
  if (!g_ov.physical) VkLog("overlay: physical device", (uint64_t)(uintptr_t)physical, 0, 0);
  g_ov.physical = physical;
}

static void OverlayNoteQueue(vk_dispatch_t queue, uint32_t family) {
  for (int i = 0; i < 16; ++i)
    if (!g_ov.queues[i].queue || g_ov.queues[i].queue == queue) {
      g_ov.queues[i].queue = queue;
      g_ov.queues[i].family = family;
      return;
    }
}

static void OverlayNoteSwapchain(vk_dispatch_t device, vk_handle_t swapchain, uint32_t width, uint32_t height,
                                 uint32_t format) {
  if (g_ov.swapchain == swapchain) return;
  g_ov.device = device;
  g_ov.swapchain = swapchain;
  g_ov.width = width;
  g_ov.height = height;
  g_ov.format = format;
  g_ov.image_count = 0;  // fetched again at the next present
}

static void OverlayForgetSwapchain(vk_handle_t swapchain) {
  if (g_ov.swapchain == swapchain) {
    g_ov.swapchain = 0;
    g_ov.image_count = 0;
  }
}

// Everything the copies need, made once on the presenting queue's family.
static BOOL OverlayCreate(vk_dispatch_t queue) {
  if (g_ov.ready) return g_ov.ready_queue == queue;
  if (!g_ov.physical) {
    OverlayFail("no physical device seen yet", 0);
    return 0;
  }
  if (!OverlayLoadFunctions(g_vk_modules[0].module)) return 0;
  vk_dispatch_t device = g_ov.device;
  int32_t result;
  vk_command_pool_create_info_t pool_info = {39, NULL, 0x2 /* RESET_COMMAND_BUFFER */, QueueFamily(queue)};
  if ((result = g_vkf.create_command_pool(device, &pool_info, NULL, &g_ov.pool))) {
    OverlayFail("vkCreateCommandPool", result);
    return 0;
  }
  vk_command_buffer_allocate_info_t allocate = {40, NULL, g_ov.pool, 0, kOvMaxImages};
  if ((result = g_vkf.allocate_command_buffers(device, &allocate, g_ov.command_buffers))) {
    OverlayFail("vkAllocateCommandBuffers", result);
    return 0;
  }
  for (int i = 0; i < kOvMaxImages; ++i) {
    vk_simple_create_info_t fence_info = {8, NULL, 0x1 /* SIGNALED */};
    vk_simple_create_info_t semaphore_info = {9, NULL, 0};
    if ((result = g_vkf.create_fence(device, &fence_info, NULL, &g_ov.fences[i])) ||
        (result = g_vkf.create_semaphore(device, &semaphore_info, NULL, &g_ov.semaphores[i]))) {
      OverlayFail("vkCreateFence/vkCreateSemaphore", result);
      return 0;
    }
  }
  // The staging buffer, host-visible and coherent, as large as the largest frame taken.
  vk_buffer_create_info_t buffer_info = {12, NULL, 0, (uint64_t)kOvMaxWidth * kOvMaxHeight * 4, 0x1 /* TRANSFER_SRC */,
                                         0, 0, NULL};
  if ((result = g_vkf.create_buffer(device, &buffer_info, NULL, &g_ov.staging))) {
    OverlayFail("vkCreateBuffer", result);
    return 0;
  }
  vk_memory_requirements_t requirements;
  g_vkf.get_buffer_memory_requirements(device, g_ov.staging, &requirements);
  static vk_physical_device_memory_properties_t properties;
  g_vkf.get_physical_device_memory_properties(g_ov.physical, &properties);
  uint32_t type = ~0u;
  for (uint32_t i = 0; i < properties.memory_type_count && i < 32; ++i)
    if ((requirements.memory_type_bits & (1u << i)) && (properties.memory_types[i].property_flags & 0x6) == 0x6) {
      type = i;
      break;
    }
  if (type == ~0u) {
    OverlayFail("no host-visible coherent memory type", 0);
    return 0;
  }
  vk_memory_allocate_info_t memory_info = {5, NULL, requirements.size, type};
  if ((result = g_vkf.allocate_memory(device, &memory_info, NULL, &g_ov.staging_memory)) ||
      (result = g_vkf.bind_buffer_memory(device, g_ov.staging, g_ov.staging_memory, 0)) ||
      (result = g_vkf.map_memory(device, g_ov.staging_memory, 0, requirements.size, 0, &g_ov.staging_map))) {
    OverlayFail("staging memory", result);
    return 0;
  }
  g_ov.ready = 1;
  g_ov.ready_queue = queue;
  VkLog("overlay ready: queue family, memory type, staging bytes", QueueFamily(queue), type, requirements.size);
  return 1;
}

static BOOL OverlayFetchImages(void) {
  if (g_ov.image_count) return 1;
  uint32_t count = kOvMaxImages;
  int32_t result = g_vkf.get_swapchain_images(g_ov.device, g_ov.swapchain, &count, g_ov.images);
  if ((result != 0 && result != 5 /* VK_INCOMPLETE */) || !count) {
    OverlayFail("vkGetSwapchainImagesKHR", result);
    return 0;
  }
  g_ov.image_count = count;
  VkLog("overlay: swapchain images, size", count, ((uint64_t)g_ov.width << 32) | g_ov.height, g_ov.format);
  return 1;
}

// Waits for every copy in flight, so the staging buffer can be written.
static BOOL OverlayIdle(void) {
  for (uint32_t i = 0; i < g_ov.image_count && i < kOvMaxImages; ++i)
    if (g_vkf.wait_for_fences(g_ov.device, 1, &g_ov.fences[i], 1, 200000000ull) != 0) return 0;
  return 1;
}

// Before the game's present: copies CEF's frame into the image, and returns the present info to
// use instead of the game's (waiting on the copy), or NULL to present the game's frame as it is.
static const present_info_t* OverlayPresent(void* queue, const present_info_t* info) {
  static present_info_t replacement;
  static vk_handle_t wait_semaphore;
  if (g_no_overlay || g_ov.failed || !info || info->swapchain_count != 1 || !g_ov.swapchain ||
      info->swapchains[0] != g_ov.swapchain || !g_frame)
    return NULL;
  if (g_ov.format != kOvFormatB8G8R8A8Unorm && g_ov.format != kOvFormatB8G8R8A8Srgb) return NULL;
  FrameLock();
  BOOL opaque = g_frame_opaque, dirty = g_frame_dirty;
  FrameUnlock();
  if (!opaque) return NULL;
  if (!OverlayCreate(queue) || !OverlayFetchImages()) return NULL;
  uint32_t index = info->indices[0];
  if (index >= g_ov.image_count) return NULL;

  if (dirty && OverlayIdle()) {
    FrameLock();
    uint32_t width = g_frame_width < g_ov.width ? g_frame_width : g_ov.width;
    uint32_t height = g_frame_height < g_ov.height ? g_frame_height : g_ov.height;
    unsigned char* to = g_ov.staging_map;
    for (uint32_t y = 0; y < height; ++y) {
      const unsigned char* row = g_frame + (size_t)y * g_frame_width * 4;
      for (uint32_t x = 0; x < width * 4; ++x) to[(size_t)y * width * 4 + x] = row[x];
    }
    g_ov.staging_width = width;
    g_ov.staging_height = height;
    g_frame_dirty = 0;
    FrameUnlock();
  }
  if (!g_ov.staging_width) return NULL;

  vk_dispatch_t command_buffer = g_ov.command_buffers[index];
  if (g_vkf.wait_for_fences(g_ov.device, 1, &g_ov.fences[index], 1, 200000000ull) != 0) return NULL;
  g_vkf.reset_fences(g_ov.device, 1, &g_ov.fences[index]);
  g_vkf.reset_command_buffer(command_buffer, 0);
  vk_command_buffer_begin_info_t begin = {42, NULL, 0x1 /* ONE_TIME_SUBMIT */, NULL};
  int32_t result = g_vkf.begin_command_buffer(command_buffer, &begin);
  if (result) {
    OverlayFail("vkBeginCommandBuffer", result);
    return NULL;
  }
  vk_image_memory_barrier_t to_transfer = {45, NULL, 0, kOvAccessTransferWrite, kOvLayoutUndefined,
                                           kOvLayoutTransferDst, ~0u, ~0u, g_ov.images[index], 0x1, 0, 1, 0, 1};
  g_vkf.cmd_pipeline_barrier(command_buffer, kOvStageTransfer, kOvStageTransfer, 0, 0, NULL, 0, NULL, 1,
                             &to_transfer);
  vk_buffer_image_copy_t region = {0, g_ov.staging_width, g_ov.staging_height, 0x1, 0, 0, 1, 0, 0, 0,
                                   g_ov.staging_width, g_ov.staging_height, 1};
  g_vkf.cmd_copy_buffer_to_image(command_buffer, g_ov.staging, g_ov.images[index], kOvLayoutTransferDst, 1, &region);
  vk_image_memory_barrier_t to_present = {45, NULL, kOvAccessTransferWrite, 0, kOvLayoutTransferDst,
                                          kOvLayoutPresent, ~0u, ~0u, g_ov.images[index], 0x1, 0, 1, 0, 1};
  g_vkf.cmd_pipeline_barrier(command_buffer, kOvStageTransfer, kOvStageBottom, 0, 0, NULL, 0, NULL, 1, &to_present);
  if ((result = g_vkf.end_command_buffer(command_buffer))) {
    OverlayFail("vkEndCommandBuffer", result);
    return NULL;
  }
  // After the game's rendering (the semaphores its present would wait on), before the present.
  uint32_t stages[16];
  uint32_t waits = info->wait_count < 16 ? info->wait_count : 16;
  for (uint32_t i = 0; i < waits; ++i) stages[i] = kOvStageTransfer;
  vk_submit_info_t submit = {4,     NULL, waits, (const vk_handle_t*)info->waits, stages, 1, &command_buffer, 1,
                             &g_ov.semaphores[index]};
  if ((result = g_vkf.queue_submit(queue, 1, &submit, g_ov.fences[index]))) {
    OverlayFail("vkQueueSubmit", result);
    return NULL;
  }
  replacement = *info;
  wait_semaphore = g_ov.semaphores[index];
  replacement.wait_count = 1;
  replacement.waits = &wait_semaphore;
  if (++g_ov.overlaid == 1 || g_ov.overlaid % 600 == 0)
    VkLog("overlay: menu frames drawn over the game's", g_ov.overlaid, index, 0);
  return &replacement;
}
