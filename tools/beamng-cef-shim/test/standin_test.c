// SPDX-License-Identifier: GPL-3.0-or-later
//
// Real-Vulkan test of standin.c under Wine: a main window with a real swapchain, and a stand-in
// for a second window's, used the way a game would through vk_hook.c's dispatcher: its images, an
// acquire, the empty submit that pays the acquire's semaphore before the game's submit, presents of
// both together and of the stand-in alone, a debug name, a fence from an acquire, and its destroy.
// run.sh runs it on lavapipe with the Khronos validation layer (synchronization checks on).
#include <stdint.h>
#include <stddef.h>
typedef void* HANDLE; typedef void* HMODULE; typedef int BOOL; typedef unsigned long DWORD; typedef uint16_t WCHAR;
#define WINAPI __stdcall
#define PAGE_READWRITE 0x04
__declspec(dllimport) void* WINAPI VirtualAlloc(void*, size_t, DWORD, DWORD);
__declspec(dllimport) void WINAPI Sleep(DWORD);
__declspec(dllimport) void* WINAPI GetProcAddress(HMODULE, const char*);
__declspec(dllimport) HMODULE WINAPI LoadLibraryA(const char*);
__declspec(dllimport) HMODULE WINAPI GetModuleHandleA(const char*);
__declspec(dllimport) HANDLE WINAPI GetStdHandle(DWORD);
__declspec(dllimport) BOOL WINAPI WriteFile(HANDLE, const void*, DWORD, DWORD*, void*);
__declspec(dllimport) void WINAPI ExitProcess(unsigned);
__declspec(dllimport) DWORD WINAPI GetEnvironmentVariableA(const char*, char*, DWORD);
void* memcpy(void* to, const void* from, size_t count) {
  unsigned char* t = to; const unsigned char* f = from; while (count--) *t++ = *f++; return to;
}
void* memset(void* to, int value, size_t count) { unsigned char* t = to; while (count--) *t++ = (unsigned char)value; return to; }
static size_t Length(const char* s) { size_t n = 0; while (s[n]) ++n; return n; }
static void Log(const char* t) { DWORD w; WriteFile(GetStdHandle((DWORD)-11), t, (DWORD)Length(t), &w, 0); }
static void VkLog(const char* what, uint64_t a, uint64_t b, uint64_t c) {
  char line[200]; size_t n = 0; const char* h = "test: vk "; while (*h) line[n++] = *h++; while (*what) line[n++] = *what++;
  uint64_t v3[3] = {a, b, c};
  for (int k = 0; k < 3; ++k) { char d[20]; int m = 0; uint64_t v = v3[k]; do d[m++] = "0123456789abcdef"[v & 15], v >>= 4; while (v); line[n++] = ' '; while (m) line[n++] = d[--m]; }
  line[n++] = '\n'; line[n] = 0; Log(line);
}
typedef struct { uint32_t s_type; const void* next; uint32_t wait_count; const void* waits; uint32_t swapchain_count;
                 const uint64_t* swapchains; const uint32_t* indices; int32_t* results; } present_info_t;
/* what vk_hook.c gives standin.c */
typedef long (*unix_call_fn)(uint64_t handle, unsigned int code, void* args);
static volatile long g_lock;
static void VkLock(void) { while (__sync_lock_test_and_set(&g_lock, 1)) {} }
static void VkUnlock(void) { __sync_lock_release(&g_lock); }
static long Reached(uint64_t handle, unsigned code, void* args) {
  (void)handle; (void)args; VkLog("FAILED, a call for a stand-in reached Wine, code", code, 0, 0); return 0;
}
static unix_call_fn RealFor(uint64_t handle) { (void)handle; return Reached; }
static struct { HMODULE module; } g_vk_modules[1];
static int g_vk_module_count = 1;
static BOOL g_menu_copy;
static unsigned g_code_get_images = 0x291, g_code_submit = 0x2ae, g_code_submit2 = 0x2af;
#include "../overlay.c"
#include "../standin.c"

/* user32 for the main window */
typedef struct { uint32_t style; void* proc; int extra1, extra2; HMODULE instance; void* icon; void* cursor; void* brush; const WCHAR* menu; const WCHAR* name; } WNDCLASSW;
__declspec(dllimport) uint16_t WINAPI RegisterClassW(const WNDCLASSW*);
__declspec(dllimport) void* WINAPI CreateWindowExW(DWORD, const WCHAR*, const WCHAR*, DWORD, int, int, int, int, void*, void*, HMODULE, void*);
__declspec(dllimport) intptr_t WINAPI DefWindowProcW(void*, unsigned, uintptr_t, intptr_t);
__declspec(dllimport) BOOL WINAPI ShowWindow(void*, int);
__declspec(dllimport) BOOL WINAPI PeekMessageW(void*, void*, unsigned, unsigned, unsigned);
__declspec(dllimport) BOOL WINAPI DispatchMessageW(const void*);
static intptr_t WINAPI Proc(void* w, unsigned m, uintptr_t a, intptr_t b) { return DefWindowProcW(w, m, a, b); }
static void Pump(void) { unsigned char msg[64]; while (PeekMessageW(msg, 0, 0, 0, 1)) DispatchMessageW(msg); }

#include <vulkan/vulkan_core.h>
/* the offsets standin.c reads, against the real structures */
_Static_assert(offsetof(VkSwapchainCreateInfoKHR, flags) == 16, "flags");
_Static_assert(offsetof(VkSwapchainCreateInfoKHR, minImageCount) == 32, "minImageCount");
_Static_assert(offsetof(VkSwapchainCreateInfoKHR, imageFormat) == 36, "imageFormat");
_Static_assert(offsetof(VkSwapchainCreateInfoKHR, imageExtent) == 44, "imageExtent");
_Static_assert(offsetof(VkSwapchainCreateInfoKHR, imageArrayLayers) == 52, "imageArrayLayers");
_Static_assert(offsetof(VkSwapchainCreateInfoKHR, imageUsage) == 56, "imageUsage");
_Static_assert(offsetof(VkSwapchainCreateInfoKHR, imageSharingMode) == 60, "imageSharingMode");
_Static_assert(offsetof(VkSwapchainCreateInfoKHR, queueFamilyIndexCount) == 64, "queueFamilyIndexCount");
_Static_assert(offsetof(VkSwapchainCreateInfoKHR, pQueueFamilyIndices) == 72, "pQueueFamilyIndices");
_Static_assert(offsetof(VkSwapchainCreateInfoKHR, oldSwapchain) == 96, "oldSwapchain");
_Static_assert(offsetof(VkDebugUtilsObjectNameInfoEXT, objectHandle) == 24, "debug utils name");
_Static_assert(offsetof(VkDebugUtilsObjectTagInfoEXT, objectHandle) == 24, "debug utils tag");
_Static_assert(offsetof(VkDebugMarkerObjectNameInfoEXT, object) == 24, "debug marker name");
_Static_assert(offsetof(VkDebugMarkerObjectTagInfoEXT, object) == 24, "debug marker tag");
_Static_assert(sizeof(VkHdrMetadataEXT) == 64, "VkHdrMetadataEXT");
_Static_assert(sizeof(present_info_t) == sizeof(VkPresentInfoKHR), "present_info_t");
_Static_assert(sizeof(vk_submit_info_t) == sizeof(VkSubmitInfo), "vk_submit_info_t");

typedef struct { VkStructureType sType; const void* pNext; uint32_t flags; HMODULE hinstance; void* hwnd; } SurfaceInfo;
#define LOAD(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
#define CHECK(x) do { VkResult r_ = (x); if (r_) { VkLog("FAILED " #x, (uint64_t)(uint32_t)r_, 0, 0); ExitProcess(2); } } while (0)
#define EXPECT(cond, what) do { if (!(cond)) { Log("test: FAILED, " what "\n"); ExitProcess(3); } } while (0)

static VkCommandBuffer g_cb;
static PFN_vkCmdPipelineBarrier g_barrier;
static PFN_vkCmdClearColorImage g_clear;

// A clear of image, from UNDEFINED to PRESENT_SRC, as the game draws a frame.
static void Draw(VkImage image, float red, float green, float blue) {
  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageMemoryBarrier to_clear = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ~0u, ~0u, image, range};
  g_barrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &to_clear);
  VkClearColorValue colour = {{red, green, blue, 1}};
  g_clear(g_cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &colour, 1, &range);
  VkImageMemoryBarrier to_present = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, ~0u, ~0u, image, range};
  g_barrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &to_present);
}

void mainCRTStartup(void) {
  WCHAR cls[] = {'t', 0};
  WNDCLASSW wc = {0, (void*)Proc, 0, 0, GetModuleHandleA(0), 0, 0, 0, 0, cls};
  RegisterClassW(&wc);
  void* hwnd = CreateWindowExW(0, cls, cls, 0x80000000 /* WS_POPUP */, 0, 0, 640, 360, 0, 0, GetModuleHandleA(0), 0);
  ShowWindow(hwnd, 5); Pump();
  HMODULE vk = LoadLibraryA("vulkan-1.dll");
  PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
  g_vk_modules[0].module = GetModuleHandleA("winevulkan.dll");
  EXPECT(g_vk_modules[0].module, "no winevulkan.dll");
  VkInstance instance = 0;
  PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(0, "vkCreateInstance");
  const char* iext[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "t", 1, "t", 1, VK_API_VERSION_1_1};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app, 0, 0, 2, iext};
  CHECK(vkCreateInstance(&ici, 0, &instance));
  LOAD(vkEnumeratePhysicalDevices); LOAD(vkCreateDevice); LOAD(vkGetDeviceQueue); LOAD(vkCreateSwapchainKHR);
  LOAD(vkGetSwapchainImagesKHR); LOAD(vkAcquireNextImageKHR); LOAD(vkQueuePresentKHR); LOAD(vkCreateSemaphore);
  LOAD(vkCreateCommandPool); LOAD(vkAllocateCommandBuffers); LOAD(vkBeginCommandBuffer); LOAD(vkEndCommandBuffer);
  LOAD(vkCmdPipelineBarrier); LOAD(vkCmdClearColorImage); LOAD(vkQueueSubmit); LOAD(vkDeviceWaitIdle); LOAD(vkCreateFence);
  LOAD(vkWaitForFences); LOAD(vkResetFences); LOAD(vkResetCommandBuffer); LOAD(vkDestroySwapchainKHR);
  VkResult (*vkCreateWin32SurfaceKHR)(VkInstance, const SurfaceInfo*, const void*, VkSurfaceKHR*) = (void*)gipa(instance, "vkCreateWin32SurfaceKHR");
  g_barrier = vkCmdPipelineBarrier; g_clear = vkCmdClearColorImage;
  uint32_t count = 1; VkPhysicalDevice phys; CHECK(vkEnumeratePhysicalDevices(instance, &count, &phys));
  float prio = 1; VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio};
  const char* dext[] = {"VK_KHR_swapchain"};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qci, 0, 0, 1, dext, 0};
  VkDevice device; CHECK(vkCreateDevice(phys, &dci, 0, &device));
  VkQueue queue; vkGetDeviceQueue(device, 0, 0, &queue);
  OverlayNotePhysical(phys);
  /* the main window's real swapchain */
  SurfaceInfo si = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, 0, 0, GetModuleHandleA(0), hwnd};
  VkSurfaceKHR surface; CHECK(vkCreateWin32SurfaceKHR(instance, &si, 0, &surface));
  VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, 0, 0, surface, 3, VK_FORMAT_B8G8R8A8_UNORM,
      VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, {640, 360}, 1, usage, VK_SHARING_MODE_EXCLUSIVE, 0, 0,
      VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, 1, 0};
  VkSwapchainKHR main_chain; CHECK(vkCreateSwapchainKHR(device, &sci, 0, &main_chain));
  uint32_t main_count = 8; VkImage main_images[8]; CHECK(vkGetSwapchainImagesKHR(device, main_chain, &main_count, main_images));
  /* a second window's, which Wine refuses on the PS5: the stand-in, made from the same kind of create info */
  VkSwapchainCreateInfoKHR popup_info = sci;
  popup_info.surface = (VkSurfaceKHR)0x1234;  /* never read */
  popup_info.imageExtent.width = 894; popup_info.imageExtent.height = 58;
  uint64_t popup = StandInCreate(device, (const unsigned char*)&popup_info);
  EXPECT(popup, "no stand-in was made");
  EXPECT(IsStandIn(popup) && !IsStandIn((uint64_t)main_chain), "IsStandIn");
  /* vkGetSwapchainImagesKHR, as the dispatcher passes it */
  struct { VkDevice device; uint64_t swapchain; uint32_t* count; VkImage* images; int32_t result; } images_call;
  uint32_t popup_count = 0; VkImage popup_images[8];
  images_call = (typeof(images_call)){device, popup, &popup_count, 0, -1};
  EXPECT(StandInCall(0, g_code_get_images, &images_call) && images_call.result == 0 && popup_count == 3, "image count");
  popup_count = 2;
  images_call = (typeof(images_call)){device, popup, &popup_count, popup_images, -1};
  EXPECT(StandInCall(0, g_code_get_images, &images_call) && images_call.result == VK_INCOMPLETE && popup_count == 2, "incomplete");
  popup_count = 8;
  images_call = (typeof(images_call)){device, popup, &popup_count, popup_images, -1};
  EXPECT(StandInCall(0, g_code_get_images, &images_call) && images_call.result == 0 && popup_count == 3, "images");
  /* a debug name for the stand-in stays here; one for the main swapchain goes on */
  VkDebugUtilsObjectNameInfoEXT name = {VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT, 0, VK_OBJECT_TYPE_SWAPCHAIN_KHR, popup, "popup"};
  struct { VkDevice device; const void* info; int32_t result; } name_call = {device, &name, -1};
  EXPECT(StandInCall(0, 0x2c0, &name_call) && name_call.result == 0, "debug name of the stand-in");
  name.objectHandle = (uint64_t)main_chain;
  EXPECT(!StandInCall(0, 0x2c0, &name_call), "debug name of the main swapchain");

  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
  VkCommandPool pool; CHECK(vkCreateCommandPool(device, &pci, 0, &pool));
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, 0, 1};
  CHECK(vkAllocateCommandBuffers(device, &cai, &g_cb));
  VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, 0, 0};
  VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, 0};
  VkFence done; CHECK(vkCreateFence(device, &fci, 0, &done));
  for (int f = 0; f < 24; f++) {
    VkSemaphore main_acquired, popup_acquired, main_drawn, popup_drawn;
    CHECK(vkCreateSemaphore(device, &sem, 0, &main_acquired)); CHECK(vkCreateSemaphore(device, &sem, 0, &popup_acquired));
    CHECK(vkCreateSemaphore(device, &sem, 0, &main_drawn)); CHECK(vkCreateSemaphore(device, &sem, 0, &popup_drawn));
    uint32_t mi, pi = 99; int32_t result = -1;
    CHECK(vkAcquireNextImageKHR(device, main_chain, ~0ull, main_acquired, 0, &mi));
    EXPECT(StandInAcquire(popup, (uint64_t)popup_acquired, 0, &pi, &result) && result == 0 && pi == (uint32_t)f % 3, "acquire");
    /* the game's submit: the dispatcher pays what the acquire owes first, on the game's queue */
    StandInCall(0, g_code_submit, &queue);
    EXPECT(!g_owed, "the acquire's semaphore is still owed");
    CHECK(vkResetCommandBuffer(g_cb, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, 0};
    CHECK(vkBeginCommandBuffer(g_cb, &bi));
    Draw(main_images[mi], 0, 0, 1);
    Draw(popup_images[pi], 1, 0, 0);
    CHECK(vkEndCommandBuffer(g_cb));
    VkSemaphore waits[2] = {main_acquired, popup_acquired}, signals[2] = {main_drawn, popup_drawn};
    VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkSubmitInfo s = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 2, waits, stages, 1, &g_cb, 2, signals};
    CHECK(vkQueueSubmit(queue, 1, &s, done));
    if (f % 2 == 0) {
      /* both in one present: the main one goes on with both semaphores, the stand-in is dropped */
      uint64_t chains[2] = {(uint64_t)main_chain, popup}; uint32_t indices[2] = {mi, pi}; int32_t results[2] = {-1, -1};
      present_info_t asked = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 2, signals, 2, chains, indices, results};
      present_info_t without; uint64_t kept_chains[16]; uint32_t kept_indices[16]; int32_t kept_results[16];
      EXPECT(StandInFilter(&asked, &without, kept_chains, kept_indices, kept_results) == 1 && without.swapchains[0] == (uint64_t)main_chain, "filter");
      CHECK(vkQueuePresentKHR(queue, (const VkPresentInfoKHR*)&without));
      StandInResults(&asked, &without);
      EXPECT(results[0] == 0 && results[1] == 0, "results");
    } else {
      /* each on its own: the stand-in's present only waits on its semaphore */
      present_info_t main_present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &main_drawn, 1, (const uint64_t*)&main_chain, &mi, 0};
      present_info_t without; uint64_t kept_chains[16]; uint32_t kept_indices[16]; int32_t kept_results[16];
      EXPECT(StandInFilter(&main_present, &without, kept_chains, kept_indices, kept_results) == -1, "filter of the main one");
      CHECK(vkQueuePresentKHR(queue, (const VkPresentInfoKHR*)&main_present));
      int32_t result1 = -1;
      present_info_t popup_present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &popup_drawn, 1, &popup, &pi, &result1};
      EXPECT(StandInFilter(&popup_present, &without, kept_chains, kept_indices, kept_results) == 0, "filter of the stand-in");
      StandInConsume(queue, &popup_present);
      EXPECT(result1 == 0, "stand-in result");
    }
    CHECK(vkWaitForFences(device, 1, &done, 1, ~0ull)); CHECK(vkResetFences(device, 1, &done));
    CHECK(vkDeviceWaitIdle(device));
    Pump();
  }
  /* an acquire with a fence: paid by an empty submit with that fence */
  uint32_t index; int32_t result = -1;
  EXPECT(StandInAcquire(popup, 0, (uint64_t)done, &index, &result) && result == 0, "acquire with a fence");
  StandInCall(0, g_code_submit2, &queue);
  CHECK(vkWaitForFences(device, 1, &done, 1, 2000000000ull));
  CHECK(vkDeviceWaitIdle(device));
  EXPECT(StandInDestroy(popup) && !IsStandIn(popup) && !StandInDestroy(popup), "destroy");
  vkDestroySwapchainKHR(device, main_chain, 0);
  Log("test: done\n");
  ExitProcess(0);
}
int _fltused = 0;
