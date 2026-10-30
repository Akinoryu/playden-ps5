// SPDX-License-Identifier: GPL-3.0-or-later
//
// Menu overlay and scaling, included by shim.c after vk_hook.c and paint_hook.c.
//
// On the console CEF draws BeamNG's menus correctly (frames saved by
// paint_hook.c), BeamNG gets every frame through OnPaint and presents at 60 fps,
// yet the screen stays black. So while CEF's latest frame is opaque (loading
// screens and menus cover the whole screen), the shim copies it into each
// swapchain image just before the game presents it: one buffer-to-image copy on
// the presenting queue, after the game's rendering (its present semaphores) and
// before the present (a semaphore of the shim's). While the UI is see-through (a
// HUD over the game) the game's own frame is shown. shim:no-overlay turns it off.
//
// That alone still showed black (build 15): the 1280x720 swapchain went through
// Mesa's scaled path, its own images blitted into 1920x1080 framebuffers. So
// vk_hook.c makes the swapchain at the full size (1920x1080, or shim:vk-size=),
// whose images are VideoOut's framebuffers, and here each present scales the
// game's frame, the part of the image it drew at the size it asked for, or the
// menu, up to the whole image: copied into an image of the shim's first, then
// blitted back over the whole swapchain image. shim:no-vk-fullsize keeps the
// game's size.
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
  uint32_t s_type;
  const void* next;
  uint32_t flags, image_type, format;
  uint32_t width, height, depth;
  uint32_t mip_levels, array_layers, samples, tiling, usage, sharing_mode;
  uint32_t queue_family_index_count;
  const uint32_t* queue_family_indices;
  uint32_t initial_layout;
} vk_image_create_info_t;

typedef struct {
  uint32_t aspect_mask, mip_level, base_array_layer, layer_count;
} vk_subresource_layers_t;

typedef struct {
  vk_subresource_layers_t src;
  int32_t src_x, src_y, src_z;
  vk_subresource_layers_t dst;
  int32_t dst_x, dst_y, dst_z;
  uint32_t width, height, depth;
} vk_image_copy_t;

typedef struct {
  vk_subresource_layers_t src;
  int32_t src_offsets[2][3];
  vk_subresource_layers_t dst;
  int32_t dst_offsets[2][3];
} vk_image_blit_t;

typedef struct {
  uint32_t aspect_mask, base_mip_level, level_count, base_array_layer, layer_count;
} vk_subresource_range_t;

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
  kOvLayoutTransferSrc = 6,
  kOvLayoutTransferDst = 7,
  kOvLayoutPresent = 1000001002,
  kOvStageTransfer = 0x1000,
  kOvStageBottom = 0x2000,
  kOvAccessTransferRead = 0x800,
  kOvAccessTransferWrite = 0x1000,
  kOvFormatB8G8R8A8Unorm = 44,
  kOvFormatB8G8R8A8Srgb = 50,
  kOvFilterLinear = 1,
  kOvMaxWidth = 1920,  // the largest UI frame the overlay takes, and the largest game frame it scales
  kOvMaxHeight = 1080,
};

// The size swapchains are made at (shim:vk-size=), and whether they are (shim:no-vk-fullsize).
static BOOL g_vk_fullsize = 1;
static uint32_t g_full_width = 1920, g_full_height = 1080;

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
  int32_t (*create_image)(vk_dispatch_t, const void*, const void*, vk_handle_t*);
  void (*get_image_memory_requirements)(vk_dispatch_t, vk_handle_t, void*);
  int32_t (*bind_image_memory)(vk_dispatch_t, vk_handle_t, vk_handle_t, uint64_t);
  void (*cmd_copy_image)(vk_dispatch_t, vk_handle_t, uint32_t, vk_handle_t, uint32_t, uint32_t, const void*);
  void (*cmd_blit_image)(vk_dispatch_t, vk_handle_t, uint32_t, vk_handle_t, uint32_t, uint32_t, const void*,
                         uint32_t);
  void (*cmd_clear_color_image)(vk_dispatch_t, vk_handle_t, uint32_t, const void*, uint32_t, const void*);
} g_vkf;

static struct {
  BOOL failed;            // something did not work: the overlay is off for good
  vk_dispatch_t physical;  // from vkGetPhysicalDeviceSurfaceCapabilitiesKHR
  vk_dispatch_t device;    // the swapchain's
  struct {
    vk_dispatch_t queue;
    uint32_t family;
  } queues[16];
  // The swapchain the images belong to, its size, and the size the game asked for and draws at.
  vk_handle_t swapchain;
  uint32_t width, height, format, image_count;
  uint32_t game_width, game_height;
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
  // What a scaled frame is copied into before it is blitted over the whole swapchain image.
  vk_handle_t scaler, scaler_memory;
  long overlaid, scaled;
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

// Called from OnPaintHook with each full-view frame, on the game's main thread (CEF's UI thread).
// Only the main view, at the game's size, is taken: other browsers (BeamNG's analog odometer) draw
// small textures. A see-through frame (a HUD over the game) is not copied, as it is not drawn.
static void OverlayTakeFrame(const void* pixels, int width, int height, int opaque_samples) {
  if (g_no_overlay || width <= 0 || height <= 0 || width > kOvMaxWidth || height > kOvMaxHeight) return;
  if (g_ov.game_width && ((uint32_t)width != g_ov.game_width || (uint32_t)height != g_ov.game_height)) return;
  BOOL opaque = opaque_samples >= 135;  // of 144: a loading screen or menu, not a HUD
  FrameLock();
  if (!opaque) {
    g_frame_opaque = 0;
    FrameUnlock();
    return;
  }
  if (!g_frame) g_frame = VirtualAlloc(NULL, (size_t)kOvMaxWidth * kOvMaxHeight * 4, 0x3000, PAGE_READWRITE);
  if (g_frame) {
    const unsigned char* from = pixels;
    size_t bytes = (size_t)width * (size_t)height * 4;
    for (size_t i = 0; i < bytes; ++i) g_frame[i] = from[i];
    g_frame_width = (uint32_t)width;
    g_frame_height = (uint32_t)height;
    g_frame_dirty = 1;
    g_frame_opaque = 1;
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
                         "vkGetPhysicalDeviceMemoryProperties", "vkGetSwapchainImagesKHR", "vkCreateImage",
                         "vkGetImageMemoryRequirements", "vkBindImageMemory", "vkCmdCopyImage", "vkCmdBlitImage",
                         "vkCmdClearColorImage"};
  _Static_assert(sizeof(names) == sizeof(g_vkf), "a name for each entry point");
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

// Whether a swapchain the game asks for at this size and format is made at the full size instead:
// the scaler image holds at most kOvMaxWidth x kOvMaxHeight of the game's frame, in its format.
static BOOL OverlayCanScale(uint32_t width, uint32_t height, uint32_t format) {
  return g_vk_fullsize && !g_no_overlay && !g_ov.failed && format == kOvFormatB8G8R8A8Unorm && width && height &&
         width <= kOvMaxWidth && height <= kOvMaxHeight && width <= g_full_width && height <= g_full_height &&
         (width != g_full_width || height != g_full_height);
}

static void OverlayNoteSwapchain(vk_dispatch_t device, vk_handle_t swapchain, uint32_t width, uint32_t height,
                                 uint32_t game_width, uint32_t game_height, uint32_t format) {
  if (g_ov.swapchain == swapchain) return;
  g_ov.device = device;
  g_ov.swapchain = swapchain;
  g_ov.width = width;
  g_ov.height = height;
  g_ov.game_width = game_width;
  g_ov.game_height = game_height;
  g_ov.format = format;
  g_ov.image_count = 0;  // fetched again at the next present
}

static void OverlayForgetSwapchain(vk_handle_t swapchain) {
  if (g_ov.swapchain == swapchain) {
    g_ov.swapchain = 0;
    g_ov.image_count = 0;
  }
}

// A memory type of bits with all of wanted's property flags, or ~0u.
static uint32_t OverlayMemoryType(uint32_t bits, uint32_t wanted) {
  static vk_physical_device_memory_properties_t properties;
  if (!properties.memory_type_count) g_vkf.get_physical_device_memory_properties(g_ov.physical, &properties);
  for (uint32_t i = 0; i < properties.memory_type_count && i < 32; ++i)
    if ((bits & (1u << i)) && (properties.memory_types[i].property_flags & wanted) == wanted) return i;
  return ~0u;
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
  uint32_t type = OverlayMemoryType(requirements.memory_type_bits, 0x6 /* HOST_VISIBLE | HOST_COHERENT */);
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

// The scaler image, made at the first scaled present: B8G8R8A8, as the swapchains it serves.
static BOOL OverlayCreateScaler(void) {
  if (g_ov.scaler) return 1;
  vk_dispatch_t device = g_ov.device;
  vk_image_create_info_t info = {14,     NULL, 0, 1 /* 2D */, kOvFormatB8G8R8A8Unorm, kOvMaxWidth, kOvMaxHeight, 1,
                                 1,      1,    1, 0 /* OPTIMAL */, 0x3 /* TRANSFER_SRC | TRANSFER_DST */,
                                 0,      0,    NULL, kOvLayoutUndefined};
  vk_handle_t image = 0, memory = 0;
  int32_t result = g_vkf.create_image(device, &info, NULL, &image);
  if (result) {
    OverlayFail("vkCreateImage", result);
    return 0;
  }
  vk_memory_requirements_t requirements;
  g_vkf.get_image_memory_requirements(device, image, &requirements);
  uint32_t type = OverlayMemoryType(requirements.memory_type_bits, 0x1 /* DEVICE_LOCAL */);
  if (type == ~0u) type = OverlayMemoryType(requirements.memory_type_bits, 0);
  vk_memory_allocate_info_t memory_info = {5, NULL, requirements.size, type};
  if (type == ~0u || (result = g_vkf.allocate_memory(device, &memory_info, NULL, &memory)) ||
      (result = g_vkf.bind_image_memory(device, image, memory, 0))) {
    OverlayFail("scaler memory", result);
    return 0;
  }
  g_ov.scaler = image;
  g_ov.scaler_memory = memory;
  VkLog("overlay: scaler image, memory type, bytes", image, type, requirements.size);
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
  if (g_ov.width != g_ov.game_width || g_ov.height != g_ov.game_height)
    VkLog("overlay: the game draws at, scaled up to", ((uint64_t)g_ov.game_width << 32) | g_ov.game_height,
          ((uint64_t)g_ov.width << 32) | g_ov.height, 0);
  return 1;
}

// Waits for every copy in flight, so the staging buffer can be written.
static BOOL OverlayIdle(void) {
  for (uint32_t i = 0; i < g_ov.image_count && i < kOvMaxImages; ++i)
    if (g_vkf.wait_for_fences(g_ov.device, 1, &g_ov.fences[i], 1, 200000000ull) != 0) return 0;
  return 1;
}

static vk_image_memory_barrier_t Barrier(vk_handle_t image, uint32_t src_access, uint32_t dst_access,
                                         uint32_t old_layout, uint32_t new_layout) {
  return (vk_image_memory_barrier_t){45, NULL, src_access, dst_access, old_layout, new_layout, ~0u, ~0u, image,
                                     0x1 /* COLOR */, 0, 1, 0, 1};
}

// The menu (ui) or the game's frame, width x height at the top left, into the scaler, then blitted
// over the whole of image, its aspect ratio kept, the bars black.
static void RecordScale(vk_dispatch_t command_buffer, vk_handle_t image, BOOL ui, uint32_t width, uint32_t height) {
  vk_image_memory_barrier_t first[2] = {
      Barrier(g_ov.scaler, 0, kOvAccessTransferWrite, kOvLayoutUndefined, kOvLayoutTransferDst),
      ui ? Barrier(image, 0, kOvAccessTransferWrite, kOvLayoutUndefined, kOvLayoutTransferDst)
         : Barrier(image, 0, kOvAccessTransferRead, kOvLayoutPresent, kOvLayoutTransferSrc)};
  g_vkf.cmd_pipeline_barrier(command_buffer, kOvStageTransfer, kOvStageTransfer, 0, 0, NULL, 0, NULL, 2, first);
  if (ui) {
    vk_buffer_image_copy_t region = {0, width, height, 0x1, 0, 0, 1, 0, 0, 0, width, height, 1};
    g_vkf.cmd_copy_buffer_to_image(command_buffer, g_ov.staging, g_ov.scaler, kOvLayoutTransferDst, 1, &region);
  } else {
    vk_image_copy_t region = {{0x1, 0, 0, 1}, 0, 0, 0, {0x1, 0, 0, 1}, 0, 0, 0, width, height, 1};
    g_vkf.cmd_copy_image(command_buffer, image, kOvLayoutTransferSrc, g_ov.scaler, kOvLayoutTransferDst, 1, &region);
  }
  vk_image_memory_barrier_t second[2] = {
      Barrier(g_ov.scaler, kOvAccessTransferWrite, kOvAccessTransferRead, kOvLayoutTransferDst, kOvLayoutTransferSrc),
      Barrier(image, 0, kOvAccessTransferWrite, kOvLayoutTransferSrc, kOvLayoutTransferDst)};
  g_vkf.cmd_pipeline_barrier(command_buffer, kOvStageTransfer, kOvStageTransfer, 0, 0, NULL, 0, NULL, ui ? 1 : 2,
                             second);

  // Fit width x height in the image, centred.
  uint32_t to_width = g_ov.width, to_height = g_ov.height;
  if ((uint64_t)width * g_ov.height > (uint64_t)height * g_ov.width)
    to_height = (uint32_t)((uint64_t)height * g_ov.width / width);
  else
    to_width = (uint32_t)((uint64_t)width * g_ov.height / height);
  int32_t x = (int32_t)(g_ov.width - to_width) / 2, y = (int32_t)(g_ov.height - to_height) / 2;
  if (to_width != g_ov.width || to_height != g_ov.height) {
    static const uint32_t black[4] = {0, 0, 0, 0x3f800000 /* 1.0f */};
    vk_subresource_range_t range = {0x1, 0, 1, 0, 1};
    g_vkf.cmd_clear_color_image(command_buffer, image, kOvLayoutTransferDst, black, 1, &range);
    vk_image_memory_barrier_t cleared =
        Barrier(image, kOvAccessTransferWrite, kOvAccessTransferWrite, kOvLayoutTransferDst, kOvLayoutTransferDst);
    g_vkf.cmd_pipeline_barrier(command_buffer, kOvStageTransfer, kOvStageTransfer, 0, 0, NULL, 0, NULL, 1, &cleared);
  }
  vk_image_blit_t blit = {{0x1, 0, 0, 1},
                          {{0, 0, 0}, {(int32_t)width, (int32_t)height, 1}},
                          {0x1, 0, 0, 1},
                          {{x, y, 0}, {x + (int32_t)to_width, y + (int32_t)to_height, 1}}};
  g_vkf.cmd_blit_image(command_buffer, g_ov.scaler, kOvLayoutTransferSrc, image, kOvLayoutTransferDst, 1, &blit,
                       kOvFilterLinear);
}

// Before the game's present: copies CEF's frame into the image, or scales the game's frame up to
// fill it, and returns the present info to use instead of the game's (waiting on that work), or
// NULL to present the game's frame as it is.
static const present_info_t* OverlayPresent(void* queue, const present_info_t* info) {
  static present_info_t replacement;
  static vk_handle_t wait_semaphore;
  if (g_ov.failed || !info || info->swapchain_count != 1 || !g_ov.swapchain || info->swapchains[0] != g_ov.swapchain)
    return NULL;
  BOOL scale = g_ov.width != g_ov.game_width || g_ov.height != g_ov.game_height;
  BOOL ui = 0, dirty = 0;
  if (!g_no_overlay && g_frame &&
      (g_ov.format == kOvFormatB8G8R8A8Unorm || g_ov.format == kOvFormatB8G8R8A8Srgb)) {
    FrameLock();
    ui = g_frame_opaque;
    dirty = g_frame_dirty;
    FrameUnlock();
  }
  if (!ui && !scale) return NULL;
  if (!OverlayCreate(queue) || !OverlayFetchImages() || (scale && !OverlayCreateScaler())) return NULL;
  uint32_t index = info->indices[0];
  if (index >= g_ov.image_count) return NULL;

  if (ui && dirty && OverlayIdle()) {
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
  if (!g_ov.staging_width) ui = 0;
  if (!ui && !scale) return NULL;

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
  vk_handle_t image = g_ov.images[index];
  if (scale) {
    RecordScale(command_buffer, image, ui, ui ? g_ov.staging_width : g_ov.game_width,
                ui ? g_ov.staging_height : g_ov.game_height);
  } else {
    vk_image_memory_barrier_t to_transfer =
        Barrier(image, 0, kOvAccessTransferWrite, kOvLayoutUndefined, kOvLayoutTransferDst);
    g_vkf.cmd_pipeline_barrier(command_buffer, kOvStageTransfer, kOvStageTransfer, 0, 0, NULL, 0, NULL, 1,
                               &to_transfer);
    vk_buffer_image_copy_t region = {0, g_ov.staging_width, g_ov.staging_height, 0x1, 0, 0, 1, 0, 0, 0,
                                     g_ov.staging_width, g_ov.staging_height, 1};
    g_vkf.cmd_copy_buffer_to_image(command_buffer, g_ov.staging, image, kOvLayoutTransferDst, 1, &region);
  }
  vk_image_memory_barrier_t to_present =
      Barrier(image, kOvAccessTransferWrite, 0, kOvLayoutTransferDst, kOvLayoutPresent);
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
  if (ui && (++g_ov.overlaid == 1 || g_ov.overlaid % 600 == 0))
    VkLog("overlay: menu frames drawn over the game's", g_ov.overlaid, index, 0);
  if (scale && (++g_ov.scaled == 1 || g_ov.scaled % 600 == 0))
    VkLog("overlay: frames scaled up, of them menus", g_ov.scaled, g_ov.overlaid, 0);
  return &replacement;
}
