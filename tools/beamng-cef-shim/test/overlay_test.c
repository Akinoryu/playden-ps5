// SPDX-License-Identifier: GPL-3.0-or-later
//
// Real-Vulkan test of overlay.c under Wine: a window, a swapchain, a "game" frame cleared to blue,
// then OverlayPresent copies a red/green test frame over it before the present. run.sh builds it,
// runs it on lavapipe with the Khronos validation layer (synchronization checks on) and checks a
// screenshot of the display.
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
static struct { HMODULE module; } g_vk_modules[1];
#include "../overlay.c"

/* user32 for the window */
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
typedef struct { VkStructureType sType; const void* pNext; uint32_t flags; HMODULE hinstance; void* hwnd; } SurfaceInfo;
#define LOAD(name) PFN_##name name = (PFN_##name)gipa(instance, #name)
#define CHECK(x) do { VkResult r_ = (x); if (r_) { VkLog("FAILED " #x, (uint64_t)(uint32_t)r_, 0, 0); ExitProcess(2); } } while (0)

void mainCRTStartup(void) {
  WCHAR cls[] = {'t', 0};
  WNDCLASSW wc = {0, (void*)Proc, 0, 0, GetModuleHandleA(0), 0, 0, 0, 0, cls};
  RegisterClassW(&wc);
  void* hwnd = CreateWindowExW(0, cls, cls, 0x80000000 /* WS_POPUP */, 0, 0, 1280, 720, 0, 0, GetModuleHandleA(0), 0);
  ShowWindow(hwnd, 5); Pump();
  HMODULE vk = LoadLibraryA("vulkan-1.dll");
  PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
  g_vk_modules[0].module = GetModuleHandleA("winevulkan.dll");
  if (!g_vk_modules[0].module) { Log("test: no winevulkan.dll\n"); ExitProcess(3); }
  VkInstance instance = 0;
  PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(0, "vkCreateInstance");
  const char* iext[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "t", 1, "t", 1, VK_API_VERSION_1_1};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app, 0, 0, 2, iext};
  CHECK(vkCreateInstance(&ici, 0, &instance));
  LOAD(vkEnumeratePhysicalDevices); LOAD(vkCreateDevice); LOAD(vkGetDeviceQueue); LOAD(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
  LOAD(vkCreateSwapchainKHR); LOAD(vkGetSwapchainImagesKHR); LOAD(vkAcquireNextImageKHR); LOAD(vkQueuePresentKHR); LOAD(vkCreateSemaphore);
  LOAD(vkCreateCommandPool); LOAD(vkAllocateCommandBuffers); LOAD(vkBeginCommandBuffer); LOAD(vkEndCommandBuffer); LOAD(vkCmdPipelineBarrier);
  LOAD(vkCmdClearColorImage); LOAD(vkQueueSubmit); LOAD(vkDeviceWaitIdle); LOAD(vkCreateFence); LOAD(vkWaitForFences); LOAD(vkResetFences);
  VkResult (*vkCreateWin32SurfaceKHR)(VkInstance, const SurfaceInfo*, const void*, VkSurfaceKHR*) = (void*)gipa(instance, "vkCreateWin32SurfaceKHR");
  uint32_t count = 1; VkPhysicalDevice phys; CHECK(vkEnumeratePhysicalDevices(instance, &count, &phys));
  float prio = 1; VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio};
  const char* dext[] = {"VK_KHR_swapchain"};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &qci, 0, 0, 1, dext, 0};
  VkDevice device; CHECK(vkCreateDevice(phys, &dci, 0, &device));
  VkQueue queue; vkGetDeviceQueue(device, 0, 0, &queue);
  SurfaceInfo si = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR, 0, 0, GetModuleHandleA(0), hwnd};
  VkSurfaceKHR surface; CHECK(vkCreateWin32SurfaceKHR(instance, &si, 0, &surface));
  VkSurfaceCapabilitiesKHR caps; CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
  VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, 0, 0, surface, 3, VK_FORMAT_B8G8R8A8_UNORM,
      VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, {1280, 720}, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      VK_SHARING_MODE_EXCLUSIVE, 0, 0, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, 1, 0};
  VkSwapchainKHR swapchain; CHECK(vkCreateSwapchainKHR(device, &sci, 0, &swapchain));
  uint32_t n = 8; VkImage images[8]; CHECK(vkGetSwapchainImagesKHR(device, swapchain, &n, images));
  /* what the shim learns from the dispatcher */
  OverlayNotePhysical(phys); OverlayNoteQueue(queue, 0); OverlayNoteSwapchain(device, (uint64_t)swapchain, 1280, 720, VK_FORMAT_B8G8R8A8_UNORM);
  /* the "menu": left half red, right half green, opaque BGRA */
  static uint32_t frame[1280 * 720];
  for (int y = 0; y < 720; y++) for (int x = 0; x < 1280; x++) frame[y * 1280 + x] = x < 640 ? 0xffff0000u : 0xff00ff00u;
  int use_overlay = 1;
  /* game: clear to blue each frame */
  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
  VkCommandPool pool; CHECK(vkCreateCommandPool(device, &pci, 0, &pool));
  VkCommandBuffer cbs[8]; VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, 0, n};
  CHECK(vkAllocateCommandBuffers(device, &cai, cbs));
  VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, 0, 0};
  VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, VK_FENCE_CREATE_SIGNALED_BIT};
  VkSemaphore acquired[8], rendered[8]; VkFence fences[8];
  for (uint32_t i = 0; i < n; i++) { CHECK(vkCreateSemaphore(device, &sem, 0, &acquired[i])); CHECK(vkCreateSemaphore(device, &sem, 0, &rendered[i])); CHECK(vkCreateFence(device, &fci, 0, &fences[i])); }
  for (int f = 0; f < 30; f++) {
    if (f == 3 && use_overlay) OverlayTakeFrame(frame, 1280, 720, 144);
    uint32_t a = (uint32_t)f % n, i;
    CHECK(vkWaitForFences(device, 1, &fences[a], 1, ~0ull)); CHECK(vkResetFences(device, 1, &fences[a]));
    CHECK(vkAcquireNextImageKHR(device, swapchain, ~0ull, acquired[a], 0, &i));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, 0, 0};
    CHECK(vkBeginCommandBuffer(cbs[a], &bi));
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier b1 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ~0u, ~0u, images[i], range};
    vkCmdPipelineBarrier(cbs[a], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, 0, 0, 0, 1, &b1);
    VkClearColorValue blue = {{0, 0, 1, 1}};
    vkCmdClearColorImage(cbs[a], images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blue, 1, &range);
    VkImageMemoryBarrier b2 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, ~0u, ~0u, images[i], range};
    vkCmdPipelineBarrier(cbs[a], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, 0, 0, 0, 1, &b2);
    CHECK(vkEndCommandBuffer(cbs[a]));
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo s = {VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 1, &acquired[a], &stage, 1, &cbs[a], 1, &rendered[a]};
    CHECK(vkQueueSubmit(queue, 1, &s, fences[a]));
    present_info_t pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, 0, 1, &rendered[a], 1, (const uint64_t*)&swapchain, &i, 0};
    const present_info_t* replacement = OverlayPresent(queue, &pi);
    CHECK(vkQueuePresentKHR(queue, (const VkPresentInfoKHR*)(replacement ? replacement : &pi)));
    Pump();
  }
  CHECK(vkDeviceWaitIdle(device));
  VkLog(g_ov.overlaid ? "overlay drew frames:" : "overlay drew nothing:", (uint64_t)g_ov.overlaid, 0, 0);
  Log("test: done, holding the window for the screenshot\n");
  for (int k = 0; k < 40; k++) { Pump(); Sleep(100); }
  ExitProcess(0);
}
int _fltused = 0;
