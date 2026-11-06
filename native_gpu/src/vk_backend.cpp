// native_gpu : backend Vulkan minimal (palier 2 : Clear + Swap). Vulkan 1.3, bibliothèque système (libvulkan),
// fenêtre = celle créée par le SDK (SDL3) : on lit son SDL_Window par SDL_GetWindows() et on crée la surface X11 /
// Wayland nous-mêmes (la fenêtre n'a pas le fanion SDL_WINDOW_VULKAN).

#include "vk_backend.h"

#include <SDL3/SDL.h>
#include <X11/Xlib.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>
#include <vulkan/vulkan_xlib.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>

#include "ngpu.h"

namespace ngpu {
namespace {

#define VKC(x)                                                              \
  do {                                                                      \
    VkResult r_ = (x);                                                      \
    if (r_ != VK_SUCCESS) { NGPU_ERR("%s -> %d (%s:%d)", #x, (int)r_, __FILE__, __LINE__); return false; } \
  } while (0)

struct State {
  std::mutex mu;
  bool init_tried = false, ok = false;
  VkInstance inst = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice dev = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t qfam = 0;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memprops{};
  // tampon d'image
  VkImage bb = VK_NULL_HANDLE;
  VkDeviceMemory bbmem = VK_NULL_HANDLE;
  VkImageLayout bblayout = VK_IMAGE_LAYOUT_UNDEFINED;
  // fenêtre
  SDL_Window* win = nullptr;
  VkSurfaceKHR surf = VK_NULL_HANDLE;
  VkSwapchainKHR sc = VK_NULL_HANDLE;
  VkFormat scfmt = VK_FORMAT_UNDEFINED;
  VkExtent2D scext{};
  std::vector<VkImage> scimgs;
  VkSemaphore sem_acq = VK_NULL_HANDLE, sem_done = VK_NULL_HANDLE;
  uint64_t presented = 0;
  bool recording = false;
  PFN_vkCreateXlibSurfaceKHR pfnXlib = nullptr;
  PFN_vkCreateWaylandSurfaceKHR pfnWl = nullptr;
};
State g;

uint32_t FindMem(uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < g.memprops.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (g.memprops.memoryTypes[i].propertyFlags & want) == want) return i;
  return ~0u;
}

bool BeginCb() {
  if (g.recording) return true;
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKC(vkResetCommandBuffer(g.cb, 0));
  VKC(vkBeginCommandBuffer(g.cb, &bi));
  g.recording = true;
  return true;
}

void Barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.oldLayout = from; b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool Submit() {
  if (!g.recording) return true;
  VKC(vkEndCommandBuffer(g.cb));
  g.recording = false;
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1; si.pCommandBuffers = &g.cb;
  VKC(vkResetFences(g.dev, 1, &g.fence));
  VKC(vkQueueSubmit(g.queue, 1, &si, g.fence));
  VKC(vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX));
  return true;
}

bool CreateInstanceDevice() {
  const char* exts[] = {"VK_KHR_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface"};
  uint32_t n = 0; vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> have(n); vkEnumerateInstanceExtensionProperties(nullptr, &n, have.data());
  std::vector<const char*> use;
  for (auto* e : exts)
    for (auto& h : have) if (!std::strcmp(h.extensionName, e)) { use.push_back(e); break; }
  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  ai.pApplicationName = "xenondash-native-gpu"; ai.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &ai; ci.enabledExtensionCount = (uint32_t)use.size(); ci.ppEnabledExtensionNames = use.data();
  VKC(vkCreateInstance(&ci, nullptr, &g.inst));
  g.pfnXlib = (PFN_vkCreateXlibSurfaceKHR)vkGetInstanceProcAddr(g.inst, "vkCreateXlibSurfaceKHR");
  g.pfnWl = (PFN_vkCreateWaylandSurfaceKHR)vkGetInstanceProcAddr(g.inst, "vkCreateWaylandSurfaceKHR");

  uint32_t pc = 0; vkEnumeratePhysicalDevices(g.inst, &pc, nullptr);
  std::vector<VkPhysicalDevice> pds(pc); vkEnumeratePhysicalDevices(g.inst, &pc, pds.data());
  if (!pc) { NGPU_ERR("aucun périphérique Vulkan"); return false; }
  g.phys = pds[0];
  for (auto p : pds) { VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(p, &pr); if (pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { g.phys = p; break; } }
  VkPhysicalDeviceProperties pr; vkGetPhysicalDeviceProperties(g.phys, &pr);
  std::fprintf(stderr, "[ngpu] Vulkan : %s (API %u.%u)\n", pr.deviceName, VK_VERSION_MAJOR(pr.apiVersion), VK_VERSION_MINOR(pr.apiVersion));
  vkGetPhysicalDeviceMemoryProperties(g.phys, &g.memprops);
  uint32_t qc = 0; vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qc, nullptr);
  std::vector<VkQueueFamilyProperties> qf(qc); vkGetPhysicalDeviceQueueFamilyProperties(g.phys, &qc, qf.data());
  g.qfam = ~0u;
  for (uint32_t i = 0; i < qc; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g.qfam = i; break; }
  if (g.qfam == ~0u) return false;
  float prio = 1.f;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = g.qfam; qi.queueCount = 1; qi.pQueuePriorities = &prio;
  const char* dexts[] = {"VK_KHR_swapchain"};
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = dexts;
  VKC(vkCreateDevice(g.phys, &di, nullptr, &g.dev));
  vkGetDeviceQueue(g.dev, g.qfam, 0, &g.queue);
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = g.qfam;
  VKC(vkCreateCommandPool(g.dev, &pi, nullptr, &g.pool));
  VkCommandBufferAllocateInfo ai2{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai2.commandPool = g.pool; ai2.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai2.commandBufferCount = 1;
  VKC(vkAllocateCommandBuffers(g.dev, &ai2, &g.cb));
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VKC(vkCreateFence(g.dev, &fi, nullptr, &g.fence));
  VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VKC(vkCreateSemaphore(g.dev, &si, nullptr, &g.sem_acq));
  VKC(vkCreateSemaphore(g.dev, &si, nullptr, &g.sem_done));
  return true;
}

bool CreateBackbuffer() {
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
  ii.extent = {kBackbufferWidth, kBackbufferHeight, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VKC(vkCreateImage(g.dev, &ii, nullptr, &g.bb));
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(g.dev, g.bb, &mr);
  VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ma.allocationSize = mr.size; ma.memoryTypeIndex = FindMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VKC(vkAllocateMemory(g.dev, &ma, nullptr, &g.bbmem));
  VKC(vkBindImageMemory(g.dev, g.bb, g.bbmem, 0));
  if (!BeginCb()) return false;
  Barrier(g.cb, g.bb, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  g.bblayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  return true;
}

void DestroySwapchain() {
  if (g.sc) { vkDeviceWaitIdle(g.dev); vkDestroySwapchainKHR(g.dev, g.sc, nullptr); g.sc = VK_NULL_HANDLE; g.scimgs.clear(); }
}

bool EnsureSurface() {
  if (g.surf) return true;
  if (!g.win) {
    int n = 0;
    SDL_Window** wins = SDL_GetWindows(&n);
    if (wins && n > 0) g.win = wins[0];
    SDL_free(wins);
    if (!g.win) return false;
  }
  SDL_PropertiesID pr = SDL_GetWindowProperties(g.win);
  void* xd = SDL_GetPointerProperty(pr, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
  int64_t xw = SDL_GetNumberProperty(pr, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
  void* wd = SDL_GetPointerProperty(pr, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
  void* ws = SDL_GetPointerProperty(pr, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
  if (xd && xw && g.pfnXlib) {
    VkXlibSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
    ci.dpy = (Display*)xd; ci.window = (Window)xw;
    VKC(g.pfnXlib(g.inst, &ci, nullptr, &g.surf));
  } else if (wd && ws && g.pfnWl) {
    VkWaylandSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
    ci.display = (wl_display*)wd; ci.surface = (wl_surface*)ws;
    VKC(g.pfnWl(g.inst, &ci, nullptr, &g.surf));
  } else {
    return false;
  }
  VkBool32 sup = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(g.phys, g.qfam, g.surf, &sup);
  if (!sup) { NGPU_ERR("la file graphique ne présente pas sur cette surface"); return false; }
  return true;
}

bool EnsureSwapchain() {
  if (!EnsureSurface()) return false;
  VkSurfaceCapabilitiesKHR caps;
  VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.phys, g.surf, &caps));
  VkExtent2D ext = caps.currentExtent;
  if (ext.width == 0xFFFFFFFFu) ext = {kBackbufferWidth, kBackbufferHeight};
  if (!ext.width || !ext.height) return false;
  if (g.sc && ext.width == g.scext.width && ext.height == g.scext.height) return true;
  DestroySwapchain();
  uint32_t fc = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surf, &fc, nullptr);
  std::vector<VkSurfaceFormatKHR> fmts(fc); vkGetPhysicalDeviceSurfaceFormatsKHR(g.phys, g.surf, &fc, fmts.data());
  VkSurfaceFormatKHR sf = fmts[0];
  for (auto& f : fmts) if (f.format == VK_FORMAT_B8G8R8A8_UNORM) { sf = f; break; }
  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = g.surf; ci.minImageCount = std::max(caps.minImageCount, 2u);
  if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
  ci.imageFormat = sf.format; ci.imageColorSpace = sf.colorSpace; ci.imageExtent = ext; ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform; ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // toujours disponible ; MESA_VK_WSI_PRESENT_MODE=immediate le contourne sous Mesa
  { uint32_t pm = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surf, &pm, nullptr);
    std::vector<VkPresentModeKHR> pms(pm); vkGetPhysicalDeviceSurfacePresentModesKHR(g.phys, g.surf, &pm, pms.data());
    if (std::getenv("DASH_NGPU_IMMEDIATE")) for (auto m : pms) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) ci.presentMode = m; }
  ci.clipped = VK_TRUE;
  VKC(vkCreateSwapchainKHR(g.dev, &ci, nullptr, &g.sc));
  g.scfmt = sf.format; g.scext = ext;
  uint32_t ic = 0; vkGetSwapchainImagesKHR(g.dev, g.sc, &ic, nullptr);
  g.scimgs.resize(ic); vkGetSwapchainImagesKHR(g.dev, g.sc, &ic, g.scimgs.data());
  return true;
}

}  // namespace

bool VkInit() {
  std::lock_guard<std::mutex> l(g.mu);
  if (g.init_tried) return g.ok;
  g.init_tried = true;
  g.ok = CreateInstanceDevice() && CreateBackbuffer();
  if (!g.ok) NGPU_ERR("initialisation Vulkan impossible");
  return g.ok;
}

void VkClear(const float rgba[4]) {
  if (!VkInit()) return;
  std::lock_guard<std::mutex> l(g.mu);
  if (!BeginCb()) return;
  Barrier(g.cb, g.bb, g.bblayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkClearColorValue cv; for (int i = 0; i < 4; i++) cv.float32[i] = rgba[i];
  VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(g.cb, g.bb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &r);
  g.bblayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
}

uint64_t VkPresent() {
  if (!VkInit()) return 0;
  std::lock_guard<std::mutex> l(g.mu);
  if (!BeginCb()) return g.presented;
  Barrier(g.cb, g.bb, g.bblayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  g.bblayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  if (!EnsureSwapchain()) { Submit(); return g.presented; }
  uint32_t idx = 0;
  VkResult r = vkAcquireNextImageKHR(g.dev, g.sc, UINT64_MAX, g.sem_acq, VK_NULL_HANDLE, &idx);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { Submit(); DestroySwapchain(); return g.presented; }
  if (r != VK_SUCCESS) { Submit(); return g.presented; }
  VkImage si = g.scimgs[idx];
  Barrier(g.cb, si, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkImageBlit bl{};
  bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bl.dstSubresource = bl.srcSubresource;
  bl.srcOffsets[1] = {(int32_t)kBackbufferWidth, (int32_t)kBackbufferHeight, 1};
  bl.dstOffsets[1] = {(int32_t)g.scext.width, (int32_t)g.scext.height, 1};
  vkCmdBlitImage(g.cb, g.bb, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
  Barrier(g.cb, si, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  vkEndCommandBuffer(g.cb); g.recording = false;
  VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo sub{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  sub.waitSemaphoreCount = 1; sub.pWaitSemaphores = &g.sem_acq; sub.pWaitDstStageMask = &ws;
  sub.commandBufferCount = 1; sub.pCommandBuffers = &g.cb; sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &g.sem_done;
  vkResetFences(g.dev, 1, &g.fence);
  if (vkQueueSubmit(g.queue, 1, &sub, g.fence) != VK_SUCCESS) return g.presented;
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &g.sem_done; pi.swapchainCount = 1; pi.pSwapchains = &g.sc; pi.pImageIndices = &idx;
  vkQueuePresentKHR(g.queue, &pi);
  vkWaitForFences(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);
  vkQueueWaitIdle(g.queue);
  return ++g.presented;
}

}  // namespace ngpu
