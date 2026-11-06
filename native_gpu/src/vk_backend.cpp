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
#include <chrono>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "ngpu.h"

namespace ngpu {
namespace {
bool n_log() { static bool v = std::getenv("DASH_NGPU_FRAMELOG") != nullptr; return v; }

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
  bool draw_ok = false;
  bool rendering = false;
  PFN_vkCmdBeginRendering pBegin = nullptr;
  PFN_vkCmdEndRendering pEnd = nullptr;
  PFN_vkGetBufferDeviceAddress pBda = nullptr;
  VkImageView bbview = VK_NULL_HANDLE;
  // anneau de données par image (constantes + sommets)
  VkBuffer ring = VK_NULL_HANDLE; VkDeviceMemory ringmem = VK_NULL_HANDLE; uint8_t* ringptr = nullptr; VkDeviceAddress ringaddr = 0;
  VkDeviceSize ringsize = 0, ringoff = 0;
  // anneau d'envoi de textures
  VkBuffer stg = VK_NULL_HANDLE; VkDeviceMemory stgmem = VK_NULL_HANDLE; uint8_t* stgptr = nullptr; VkDeviceSize stgsize = 0, stgoff = 0;
  VkDescriptorSetLayout dsl_tex = VK_NULL_HANDLE, dsl_samp = VK_NULL_HANDLE, dsl_unused = VK_NULL_HANDLE;
  VkPipelineLayout playout = VK_NULL_HANDLE;
  VkDescriptorPool dpool = VK_NULL_HANDLE;
  HostTexture* dummy = nullptr;
  std::unordered_map<uint64_t, VkShaderModule> modules;
  std::unordered_map<uint64_t, VkPipeline> pipelines;
  std::unordered_map<uint32_t, VkSampler> samplers;
  uint64_t draws_frame = 0, draws_total = 0;
  PFN_vkCreateXlibSurfaceKHR pfnXlib = nullptr;
  PFN_vkCreateWaylandSurfaceKHR pfnWl = nullptr;
};
State g;

uint32_t FindMem(uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < g.memprops.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (g.memprops.memoryTypes[i].propertyFlags & want) == want) return i;
  return ~0u;
}

void EndRendering() {
  if (g.rendering) { g.pEnd(g.cb); g.rendering = false; }
}

bool BeginCb() {
  if (g.recording) return true;
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKC(vkResetCommandBuffer(g.cb, 0));
  VKC(vkBeginCommandBuffer(g.cb, &bi));
  g.recording = true;
  g.rendering = false;
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
  EndRendering();
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
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f12.pNext = &f13;
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f12;
  vkGetPhysicalDeviceFeatures2(g.phys, &f2);
  if (!f12.bufferDeviceAddress || !f12.runtimeDescriptorArray || !f12.descriptorBindingPartiallyBound || !f13.dynamicRendering || !f2.features.shaderInt64 ||
      !f2.features.shaderSampledImageArrayDynamicIndexing) {
    NGPU_ERR("fonctionnalités Vulkan manquantes (bufferDeviceAddress / indexation de descripteurs / rendu dynamique / int64)");
    g.draw_ok = false;
  } else {
    g.draw_ok = true;
  }
  VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  e12.bufferDeviceAddress = f12.bufferDeviceAddress; e12.runtimeDescriptorArray = f12.runtimeDescriptorArray;
  e12.descriptorBindingPartiallyBound = f12.descriptorBindingPartiallyBound; e12.shaderSampledImageArrayNonUniformIndexing = f12.shaderSampledImageArrayNonUniformIndexing;
  e12.descriptorIndexing = f12.descriptorIndexing; e12.scalarBlockLayout = f12.scalarBlockLayout;
  VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  e13.dynamicRendering = f13.dynamicRendering;
  e12.pNext = &e13;
  VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  e2.features.shaderInt64 = f2.features.shaderInt64; e2.features.shaderSampledImageArrayDynamicIndexing = f2.features.shaderSampledImageArrayDynamicIndexing;
  e2.features.samplerAnisotropy = f2.features.samplerAnisotropy;
  e2.pNext = &e12;
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.pNext = &e2;
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

// ============================================================================================================
// Dessin : pipelines hachés, anneau de constantes/sommets, descripteurs « bindless » (modèle de XenosRecomp)
// ============================================================================================================

struct HostTextureImpl {
  VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
  uint32_t w = 0, h = 0;
};

bool MakeBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer* buf, VkDeviceMemory* mem, uint8_t** ptr, VkDeviceAddress* addr) {
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VKC(vkCreateBuffer(g.dev, &bi, nullptr, buf));
  VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g.dev, *buf, &mr);
  VkMemoryAllocateFlagsInfo fi{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  fi.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ma.pNext = addr ? &fi : nullptr;
  ma.allocationSize = mr.size;
  ma.memoryTypeIndex = FindMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VKC(vkAllocateMemory(g.dev, &ma, nullptr, mem));
  VKC(vkBindBufferMemory(g.dev, *buf, *mem, 0));
  void* p = nullptr;
  VKC(vkMapMemory(g.dev, *mem, 0, VK_WHOLE_SIZE, 0, &p));
  *ptr = (uint8_t*)p;
  if (addr) {
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = *buf; *addr = g.pBda(g.dev, &ai);
  }
  return true;
}

bool InitDraw() {
  if (!g.draw_ok) return false;
  g.pBegin = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(g.dev, "vkCmdBeginRendering");
  g.pEnd = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(g.dev, "vkCmdEndRendering");
  g.pBda = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(g.dev, "vkGetBufferDeviceAddress");
  if (!g.pBegin || !g.pEnd || !g.pBda) { g.draw_ok = false; return false; }
  g.ringsize = 32u << 20; g.stgsize = 64u << 20;
  if (!MakeBuffer(g.ringsize, VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &g.ring, &g.ringmem, &g.ringptr, &g.ringaddr)) return false;
  if (!MakeBuffer(g.stgsize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g.stg, &g.stgmem, &g.stgptr, nullptr)) return false;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = g.bb; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UNORM;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VKC(vkCreateImageView(g.dev, &vi, nullptr, &g.bbview));
  // set 0 = textures 2D[16], set 1/2 = 3D/cube (jamais liés), set 3 = échantillonneurs[16]
  auto mk = [&](VkDescriptorType t, VkDescriptorSetLayout* out) -> bool {
    VkDescriptorSetLayoutBinding b{0, t, 16, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
    VkDescriptorBindingFlags fl = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = 1; bf.pBindingFlags = &fl;
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.pNext = &bf; ci.bindingCount = 1; ci.pBindings = &b;
    VKC(vkCreateDescriptorSetLayout(g.dev, &ci, nullptr, out));
    return true;
  };
  if (!mk(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &g.dsl_tex) || !mk(VK_DESCRIPTOR_TYPE_SAMPLER, &g.dsl_samp) || !mk(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &g.dsl_unused)) return false;
  VkDescriptorSetLayout sets[4] = {g.dsl_tex, g.dsl_unused, g.dsl_unused, g.dsl_samp};
  VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 4; pl.pSetLayouts = sets; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
  VKC(vkCreatePipelineLayout(g.dev, &pl, nullptr, &g.playout));
  VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 * 4096}, {VK_DESCRIPTOR_TYPE_SAMPLER, 16 * 4096}};
  VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dp.maxSets = 8192; dp.poolSizeCount = 2; dp.pPoolSizes = ps;
  VKC(vkCreateDescriptorPool(g.dev, &dp, nullptr, &g.dpool));
  return true;
}

VkSampler GetSampler(const SamplerDesc& d) {
  uint32_t key = d.linear | (d.mip_linear << 1) | (d.addr_u << 2) | (d.addr_v << 4);
  auto it = g.samplers.find(key);
  if (it != g.samplers.end()) return it->second;
  auto addr = [](uint8_t a) {
    switch (a) { case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE; default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; }
  };
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = d.linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  si.mipmapMode = d.mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = addr(d.addr_u); si.addressModeV = addr(d.addr_v); si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.maxLod = 16.f;
  VkSampler s = VK_NULL_HANDLE;
  vkCreateSampler(g.dev, &si, nullptr, &s);
  g.samplers[key] = s;
  return s;
}

VkShaderModule GetModule(uint64_t id, const uint32_t* words, size_t n) {
  auto it = g.modules.find(id);
  if (it != g.modules.end()) return it->second;
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = n * 4; ci.pCode = words;
  VkShaderModule m = VK_NULL_HANDLE;
  if (vkCreateShaderModule(g.dev, &ci, nullptr, &m) != VK_SUCCESS) NGPU_ERR("vkCreateShaderModule a échoué (shader %016llx)", (unsigned long long)id);
  g.modules[id] = m;
  return m;
}

VkBlendFactor BF(uint8_t f) {
  switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO; case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR; case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR; case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA; case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE; default: return VK_BLEND_FACTOR_ONE;
  }
}
VkBlendOp BO(uint8_t o) {
  switch (o) { case 1: return VK_BLEND_OP_SUBTRACT; case 2: return VK_BLEND_OP_MIN; case 3: return VK_BLEND_OP_MAX; case 4: return VK_BLEND_OP_REVERSE_SUBTRACT; default: return VK_BLEND_OP_ADD; }
}

uint64_t Mix(uint64_t h, uint64_t v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h; }

VkPipeline GetPipeline(const DrawDesc& d) {
  uint64_t key = Mix(Mix(d.vs_id, d.ps_id), d.topology);
  for (uint32_t i = 0; i < d.nattrs; i++) key = Mix(key, (uint64_t)d.attrs[i].location | ((uint64_t)d.attrs[i].is_uint << 8) | ((uint64_t)d.attrs[i].offset << 16));
  key = Mix(key, d.stride);
  const BlendDesc& b = d.blend;
  key = Mix(key, b.enable | (b.src_c << 1) | (b.dst_c << 6) | (b.op_c << 11) | (b.src_a << 14) | (b.dst_a << 19) | (b.op_a << 24) | ((uint64_t)b.color_mask << 27));
  auto it = g.pipelines.find(key);
  if (it != g.pipelines.end()) return it->second;

  VkShaderModule vm = GetModule(d.vs_id, d.vs_spv, d.vs_words), pm = GetModule(d.ps_id, d.ps_spv, d.ps_words);
  if (!vm || !pm) { g.pipelines[key] = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
  VkPipelineShaderStageCreateInfo st[2]{};
  st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vm; st[0].pName = "main";
  st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = pm; st[1].pName = "main";
  VkVertexInputBindingDescription vb{0, d.stride, VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription va[16];
  for (uint32_t i = 0; i < d.nattrs; i++) va[i] = {d.attrs[i].location, 0, d.attrs[i].is_uint ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_R32G32B32A32_SFLOAT, d.attrs[i].offset};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = d.nattrs ? 1 : 0; vi.pVertexBindingDescriptions = &vb; vi.vertexAttributeDescriptionCount = d.nattrs; vi.pVertexAttributeDescriptions = va;
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = (VkPrimitiveTopology)d.topology;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState ba{};
  ba.blendEnable = b.enable ? VK_TRUE : VK_FALSE;
  ba.srcColorBlendFactor = BF(b.src_c); ba.dstColorBlendFactor = BF(b.dst_c); ba.colorBlendOp = BO(b.op_c);
  ba.srcAlphaBlendFactor = BF(b.src_a); ba.dstAlphaBlendFactor = BF(b.dst_a); ba.alphaBlendOp = BO(b.op_a);
  ba.colorWriteMask = b.color_mask;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1; cb.pAttachments = &ba;
  VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
  VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &cf;
  VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pi.pNext = &ri; pi.stageCount = 2; pi.pStages = st; pi.pVertexInputState = &vi; pi.pInputAssemblyState = &ia; pi.pViewportState = &vp;
  pi.pRasterizationState = &rs; pi.pMultisampleState = &ms; pi.pColorBlendState = &cb; pi.pDynamicState = &ds; pi.layout = g.playout;
  VkPipeline p = VK_NULL_HANDLE;
  VkResult r = vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &pi, nullptr, &p);
  if (r != VK_SUCCESS) NGPU_ERR("pipeline : %d (vs %016llx ps %016llx)", (int)r, (unsigned long long)d.vs_id, (unsigned long long)d.ps_id);
  g.pipelines[key] = p;
  return p;
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
  EndRendering();
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
  EndRendering();
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
  if (n_log()) { static const auto t0 = std::chrono::steady_clock::now(); std::fprintf(stderr, "[ngpu] image %llu t=%.2f : %llu dessins\n", (unsigned long long)g.presented + 1, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), (unsigned long long)g.draws_frame); }
  g.ringoff = 0; g.stgoff = 0; g.draws_frame = 0;
  if (g.dpool) vkResetDescriptorPool(g.dev, g.dpool, 0);
  return ++g.presented;
}


HostTexture* VkCreateTexture(uint32_t w, uint32_t h, const uint8_t* rgba) {
  if (!VkInit() || !g.draw_ok || !w || !h) return nullptr;
  std::lock_guard<std::mutex> l(g.mu);
  if (!g.ring && !InitDraw()) return nullptr;
  auto* t = new HostTextureImpl();
  t->w = w; t->h = h;
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM; ii.extent = {w, h, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL; ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(g.dev, &ii, nullptr, &t->img) != VK_SUCCESS) { delete t; return nullptr; }
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(g.dev, t->img, &mr);
  VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ma.allocationSize = mr.size; ma.memoryTypeIndex = FindMem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(g.dev, &ma, nullptr, &t->mem) != VK_SUCCESS) { delete t; return nullptr; }
  vkBindImageMemory(g.dev, t->img, t->mem, 0);
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = t->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UNORM; vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(g.dev, &vi, nullptr, &t->view);
  VkDeviceSize sz = (VkDeviceSize)w * h * 4;
  if (g.stgoff + sz > g.stgsize) Submit();
  if (sz > g.stgsize || !BeginCb()) return nullptr;
  EndRendering();
  std::memcpy(g.stgptr + g.stgoff, rgba, sz);
  Barrier(g.cb, t->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkBufferImageCopy bc{};
  bc.bufferOffset = g.stgoff; bc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; bc.imageExtent = {w, h, 1};
  vkCmdCopyBufferToImage(g.cb, g.stg, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
  Barrier(g.cb, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  g.stgoff += (sz + 255) & ~255ull;
  return reinterpret_cast<HostTexture*>(t);
}

void VkDestroyTexture(HostTexture* tp) {
  auto* t = reinterpret_cast<HostTextureImpl*>(tp);
  if (!t) return;
  std::lock_guard<std::mutex> l(g.mu);
  Submit();
  vkDestroyImageView(g.dev, t->view, nullptr); vkDestroyImage(g.dev, t->img, nullptr); vkFreeMemory(g.dev, t->mem, nullptr);
  delete t;
}

bool VkDraw(const DrawDesc& d) {
  if (!VkInit() || !g.draw_ok || !d.vcount) return false;
  if (!g.dummy) {
    static const uint8_t white[4] = {255, 255, 255, 255};
    g.dummy = VkCreateTexture(1, 1, white);
    if (!g.dummy) return false;
  }
  std::lock_guard<std::mutex> l(g.mu);
  VkPipeline pipe = GetPipeline(d);
  if (!pipe) return false;
  const VkDeviceSize vbytes = (VkDeviceSize)d.vcount * d.stride;
  const VkDeviceSize need = 4096 * 2 + 512 + vbytes + 1024;
  if (g.ringoff + need > g.ringsize) Submit();
  if (!BeginCb()) return false;
  VkDeviceSize o = (g.ringoff + 255) & ~255ull;
  const VkDeviceSize o_vs = o, o_ps = o + 4096, o_sh = o + 8192, o_vb = o + 8192 + 512;
  std::memcpy(g.ringptr + o_vs, d.vsc, 4096);
  std::memcpy(g.ringptr + o_ps, d.psc, 4096);
  uint32_t shared[128] = {};
  for (uint32_t i = 0; i < 16; i++) { shared[i] = i; shared[16 + i] = i; shared[32 + i] = i; shared[48 + i] = i; }
  std::memcpy(&shared[66], d.half_pixel, 8);  // +264 : g_HalfPixelOffset
  std::memcpy(g.ringptr + o_sh, shared, sizeof(shared));
  std::memcpy(g.ringptr + o_vb, d.vdata, vbytes);
  g.ringoff = o_vb + vbytes;

  VkDescriptorSet sets[2];
  VkDescriptorSetLayout lay[2] = {g.dsl_tex, g.dsl_samp};
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = g.dpool; ai.descriptorSetCount = 2; ai.pSetLayouts = lay;
  if (vkAllocateDescriptorSets(g.dev, &ai, sets) != VK_SUCCESS) { NGPU_ERR("pool de descripteurs plein"); return false; }
  VkDescriptorImageInfo ii[16], si[16];
  for (int i = 0; i < 16; i++) {
    auto* t = reinterpret_cast<HostTextureImpl*>(d.tex[i] ? d.tex[i] : g.dummy);
    ii[i] = {VK_NULL_HANDLE, t->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    si[i] = {GetSampler(d.samp[i]), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
  }
  VkWriteDescriptorSet w[2]{};
  w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w[0].dstSet = sets[0]; w[0].descriptorCount = 16; w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; w[0].pImageInfo = ii;
  w[1].dstSet = sets[1]; w[1].descriptorCount = 16; w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; w[1].pImageInfo = si;
  vkUpdateDescriptorSets(g.dev, 2, w, 0, nullptr);

  if (g.bblayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
    EndRendering();
    Barrier(g.cb, g.bb, g.bblayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    g.bblayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
  if (!g.rendering) {
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = g.bbview; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {kBackbufferWidth, kBackbufferHeight}}; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    g.pBegin(g.cb, &ri);
    g.rendering = true;
  }
  vkCmdBindPipeline(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  VkViewport vp{d.viewport[0], d.viewport[1], d.viewport[2], d.viewport[3], 0.f, 1.f};
  vkCmdSetViewport(g.cb, 0, 1, &vp);
  VkRect2D sc{{d.scissor[0], d.scissor[1]}, {(uint32_t)d.scissor[2], (uint32_t)d.scissor[3]}};
  vkCmdSetScissor(g.cb, 0, 1, &sc);
  vkCmdBindDescriptorSets(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.playout, 0, 1, &sets[0], 0, nullptr);
  vkCmdBindDescriptorSets(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.playout, 3, 1, &sets[1], 0, nullptr);
  uint64_t pc[3] = {g.ringaddr + o_vs, g.ringaddr + o_ps, g.ringaddr + o_sh};
  vkCmdPushConstants(g.cb, g.playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24, pc);
  VkDeviceSize vo = o_vb;
  vkCmdBindVertexBuffers(g.cb, 0, 1, &g.ring, &vo);
  vkCmdDraw(g.cb, d.vcount, 1, 0, 0);
  g.draws_frame++; g.draws_total++;
  return true;
}

}  // namespace ngpu
