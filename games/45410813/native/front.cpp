// SPDX-License-Identifier: GPL-3.0-or-later
// XenonDash - facade de rendu natif de Skate (recette 45410813) sur le noyau ngpu_core.
//
// Reprise de la couche D3D native de Skate1Native (Kurtisone/Skate1Recompiled, GPL-3.0, commit 81805f51 :
// src/native_gpu/live_renderer.cpp, shader_registry.cpp, native_gpu.cpp), adaptee a ngpu_core :
//  - le moteur est `native_gpu::Core` (peripherique plume + Renderer + source de shaders) ;
//  - les shaders viennent du cache XenosRecomp compile avec le jeu (StaticShaderProvider), cles de microcode ;
//  - le profil (adresses D3D, listes de shaders d'interface) est celui de d3d_profile.toml (d3d_profile.generated.h) ;
//  - aucune emulation GPU : pas de plugin xenos, un GPU nul (null_gpu.cpp) et un presentateur Vulkan direct (vk_backend).
// Les appels D3D (dessins, effacements, resolves) sont enregistres au fil de l'image puis rendus par un fil a part au Swap :
// sur la console le GPU ne lit sommets, indices et textures que lorsqu'il execute l'image.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/runtime.h>

#include "d3d_profile.generated.h"
#include "device.h"
#include "generated/default/skate1_init.h"
#include "ngpu.h"
#include "shader_cache.h"
#include "static_shader_provider.h"
#include "vk_backend.h"
#include "xd_front.h"

namespace ng = native_gpu;

REXCVAR_DEFINE_INT32(native_gpu_render_scale, 100, "Skate1",
                     "Render and display the scene at this percentage of the console's resolution, 200 being 1440p")
    .range(100, 200)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(native_gpu_aspect, 16, "Skate1",
                     "Picture width for a height of 9: 16 is the game's own, 21 and 32 show more on the sides")
    .range(16, 32)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(native_gpu_stats_seconds, 10, "Skate1",
                     "Log frame rate and native renderer time every this many seconds (0 = off)");
REXCVAR_DEFINE_INT32(native_gpu_gamma, 116,
                     "Skate1", "Display ramp approximated by a power, in hundredths (116 = 1.16, 100 = none)");
REXCVAR_DEFINE_STRING(native_gpu_dump_path, "", "Skate1",
                      "Write the presented frame to this PPM file (see native_gpu_dump_frame)");
REXCVAR_DEFINE_INT32(native_gpu_dump_frame, 0, "Skate1", "Frame number to write to native_gpu_dump_path (0 = none)");

namespace ngpu {
bool Verbose() { return std::getenv("DASH_NGPU_VERBOSE") != nullptr; }
bool Observe() { return false; }
bool Enabled() { return true; }
void NullGpuAttach(uint32_t dev, uint8_t* base);
}  // namespace ngpu

namespace {

using xd_profile::kDevice;
using xd_profile::kStateSize;

rex::memory::Memory* g_memory = nullptr;
std::unique_ptr<ng::Core> g_core;
ng::Renderer* g_renderer = nullptr;

// ---- surveillance des ecritures dans la memoire physique (une page de 4 Ko = un bit) ----
class PhysicalWriteWatcher : public ng::WriteWatcher {
 public:
  explicit PhysicalWriteWatcher(rex::memory::Memory* memory) : memory_(memory) {
    handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(&OnWrite, this);
  }
  ~PhysicalWriteWatcher() override { memory_->UnregisterPhysicalMemoryInvalidationCallback(handle_); }

  uint32_t Watch(uint32_t address, uint32_t size) override {
    // Les ecritures avant ce cachet sont dans ce que l'appelant lit ensuite ; les suivantes (pages protegees d'abord) ont un
    // numero plus recent.
    const uint32_t stamp = sequence_.load(std::memory_order_acquire);
    memory_->EnablePhysicalMemoryAccessCallbacks(address, size, true, false);
    return stamp;
  }

  bool Dirty(uint32_t address, uint32_t size, uint32_t stamp) override {
    if (!size) return false;
    const uint32_t first = (address & 0x1FFFFFFF) >> 12;
    const uint32_t last = std::min(kPages - 1, ((address & 0x1FFFFFFF) + size - 1) >> 12);
    for (uint32_t page = first; page <= last;) {
      if (blocks_[page >> 6].load(std::memory_order_relaxed) <= stamp) {
        page = (page | 63) + 1;
        continue;
      }
      if (pages_[page].load(std::memory_order_relaxed) > stamp) return true;
      ++page;
    }
    return false;
  }

 private:
  static constexpr uint32_t kPages = 0x20000000 >> 12;

  static std::pair<uint32_t, uint32_t> OnWrite(void* context, uint32_t start, uint32_t length, bool) {
    auto* self = static_cast<PhysicalWriteWatcher*>(context);
    // Ne pas rendre (0, UINT32_MAX) : cela cesserait de surveiller les 4 Mo voisins.
    if (!length) return {start, 0};
    const uint32_t sequence = self->sequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint32_t first = (start & 0x1FFFFFFF) >> 12;
    const uint32_t last = std::min(kPages - 1, ((start & 0x1FFFFFFF) + length - 1) >> 12);
    for (uint32_t page = first; page <= last; ++page) {
      self->pages_[page].store(sequence, std::memory_order_relaxed);
      self->blocks_[page >> 6].store(sequence, std::memory_order_relaxed);
    }
    return {start, length};
  }

  rex::memory::Memory* memory_;
  void* handle_ = nullptr;
  std::atomic<uint32_t> sequence_{1};
  std::array<std::atomic<uint32_t>, kPages> pages_{};
  std::array<std::atomic<uint32_t>, kPages / 64> blocks_{};
};
std::unique_ptr<PhysicalWriteWatcher> g_watcher;

// ---- registre des shaders ----
// Skate construit ses objets shader en place (CreateVertexShader/CreatePixelShader : objet, taille, &info, info[0] = conteneur).
// A ce moment le conteneur est intact et son microcode est celui des archives ; on cle l'objet par le XXH3 du microcode
// (XENOS_RECOMP_MICROCODE_KEY) et on le retient par adresse pour SetVertexShader/SetPixelShader.
struct ShaderObject {
  uint64_t key = 0;             // 0 : absent du cache
  uint32_t physical_size = 0;   // taille de la partie physique (constantes litterales puis microcode)
};
std::mutex g_shader_mutex;
std::unordered_map<uint32_t, ShaderObject> g_objects;
std::unordered_map<uint64_t, std::vector<uint8_t>> g_containers;  // cle -> conteneur (virtuel + physique), copie
std::unordered_set<uint64_t> g_known;                              // cles du cache de shaders
uint32_t g_misses = 0;

const uint8_t* FindShaderContainer(uint64_t key) {
  std::lock_guard<std::mutex> lock(g_shader_mutex);
  auto it = g_containers.find(key);
  return it == g_containers.end() ? nullptr : it->second.data();
}

void OnShaderCreate(uint8_t* base, uint32_t object, uint32_t container) {
  if (!object || !container || (REX_LOAD_U32(container) & 0xFFFFFFFE) != 0x102A1100) return;
  const bool pixel = !(REX_LOAD_U32(container) & 1);
  const uint32_t virtual_size = REX_LOAD_U32(container + 4);
  const uint32_t shader = container + REX_LOAD_U32(container + 0x18);
  const uint32_t size = REX_LOAD_U32(shader + 4);
  const uint64_t hash = XXH3_64bits(base + container + virtual_size + REX_LOAD_U32(shader), size);
  const bool known = g_known.count(hash) != 0;
  std::lock_guard<std::mutex> lock(g_shader_mutex);
  g_objects[object] = {known ? hash : 0, REX_LOAD_U32(container + 8)};
  if (known) {
    if (!g_containers.count(hash)) {
      g_containers.emplace(hash, std::vector<uint8_t>(base + container, base + container + virtual_size +
                                                                            REX_LOAD_U32(container + 8)));
    }
    return;
  }
  if (g_misses++ < 64) {
    REXLOG_WARN("native_gpu: {} shader {:016X} not in the shader cache (object {:08X})", pixel ? "pixel" : "vertex",
                hash, object);
  }
}

ShaderObject FindShaderObject(uint32_t object) {
  if (!object) return {};
  std::lock_guard<std::mutex> lock(g_shader_mutex);
  auto it = g_objects.find(object);
  return it == g_objects.end() ? ShaderObject{} : it->second;
}

// ---- enregistrement de l'image ----
uint32_t g_inline_vertices = 0, g_inline_primitive = 0, g_inline_count = 0, g_inline_stride = 0;

struct DeferredCall {
  enum class Kind { kDraw, kClear, kResolve } kind;
  size_t state;  // decalage de l'ombre dans l'arene de l'image
  ng::DrawCall draw;
  ng::ClearCall clear;
  ng::ResolveCall resolve;
  size_t inline_vertices = SIZE_MAX;
  size_t vs_physical = SIZE_MAX;  // copie : D3D repatche le microcode pour chaque disposition de sommets
};
struct Frame {
  std::vector<DeferredCall> calls;
  std::vector<uint8_t> arena;
  uint32_t front_buffer = 0;
  bool dump = false;
};
Frame g_recording;

size_t SnapshotState(uint8_t* base) {
  std::vector<uint8_t>& arena = g_recording.arena;
  const size_t offset = arena.size();
  arena.insert(arena.end(), base + kDevice, base + kDevice + kStateSize);
  return offset;
}

bool ReadRect(uint8_t* base, uint32_t address, int32_t rect[4]) {
  if (!address) return false;
  for (int i = 0; i < 4; ++i) rect[i] = int32_t(REX_LOAD_U32(address + 4 * i));
  return true;
}
void ReadColor(uint8_t* base, uint32_t address, float color[4]) {
  for (int c = 0; c < 4 && address; ++c) {
    const uint32_t bits = REX_LOAD_U32(address + 4 * c);
    std::memcpy(&color[c], &bits, 4);
  }
}

void RenderDeferredCalls(Frame& frame) {
  for (DeferredCall& call : frame.calls) {
    const xenos::DeviceState state{frame.arena.data() + call.state};
    switch (call.kind) {
      case DeferredCall::Kind::kDraw:
        call.draw.state = state;
        if (call.inline_vertices != SIZE_MAX) call.draw.inline_vertices = frame.arena.data() + call.inline_vertices;
        if (call.vs_physical != SIZE_MAX) call.draw.vs_physical = frame.arena.data() + call.vs_physical;
        g_renderer->Draw(call.draw);
        break;
      case DeferredCall::Kind::kClear:
        call.clear.state = state;
        g_renderer->Clear(call.clear);
        break;
      case DeferredCall::Kind::kResolve:
        call.resolve.state = state;
        g_renderer->Resolve(call.resolve);
        break;
    }
  }
}

// ---- fil de rendu : une image a la fois pendant que le jeu enregistre la suivante (le GPU de la console a une image de retard) ----
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::optional<Frame> g_queued;
bool g_busy = false;
std::atomic<uint64_t> g_rendered{0};
std::chrono::steady_clock::duration g_thread_time{};

void WritePpm10(const std::string& path, const uint32_t* pixels, uint32_t width, uint32_t height) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  std::fprintf(f, "P6\n%u %u\n255\n", width, height);
  for (size_t i = 0; i < size_t(width) * height; ++i) {
    const uint8_t rgb[3] = {uint8_t((pixels[i] & 1023) >> 2), uint8_t(((pixels[i] >> 10) & 1023) >> 2),
                            uint8_t(((pixels[i] >> 20) & 1023) >> 2)};
    std::fwrite(rgb, 1, 3, f);
  }
  std::fclose(f);
}

void RenderFrame(Frame& frame) {
  const auto start = std::chrono::steady_clock::now();
  g_renderer->SetRenderScale(float(REXCVAR_GET(native_gpu_render_scale)) / 100.0f);
  g_renderer->SetWidth(float(REXCVAR_GET(native_gpu_aspect)) / 16.0f);
  g_renderer->BeginFrame();
  RenderDeferredCalls(frame);
  g_renderer->EndFrame(frame.front_buffer);
  // Le moteur a une image de retard : presented() est l'image precedente, deja terminee.
  if (const uint32_t* pixels = g_renderer->presented()) {
    const uint32_t w = g_renderer->presented_width(), h = g_renderer->presented_height();
    const uint64_t n = ++g_rendered;
    const int32_t dump = REXCVAR_GET(native_gpu_dump_frame);
    if (dump > 0 && uint64_t(dump) == n && !REXCVAR_GET(native_gpu_dump_path).empty()) {
      WritePpm10(REXCVAR_GET(native_gpu_dump_path), pixels, w, h);
      REXLOG_INFO("native_gpu: frame {} written to {}", n, REXCVAR_GET(native_gpu_dump_path));
    }
    ngpu::VkPresentPacked(pixels, w, h);
    if (n == 1) REXLOG_INFO("native_gpu: first native frame presented ({}x{})", w, h);
  }
  g_thread_time += std::chrono::steady_clock::now() - start;
}

void LogRendererIssues() {
  static std::map<std::string, uint32_t> logged;
  for (const auto& [what, count] : g_renderer->stats()) {
    if (what.find("skipped") == std::string::npos && what.find("vertex element") == std::string::npos &&
        what.find("heap full") == std::string::npos) {
      continue;
    }
    uint32_t& last = logged[what];
    if (count >= last * 10 + 1) {
      last = count;
      REXLOG_WARN("native_gpu: {} ({}x)", what, count);
    }
  }
}

void RendererThread() {
  for (;;) {
    Frame frame;
    {
      std::unique_lock<std::mutex> lock(g_queue_mutex);
      g_queue_cv.wait(lock, [] { return g_queued.has_value(); });
      frame = std::move(*g_queued);
      g_queued.reset();
      g_busy = true;
    }
    RenderFrame(frame);
    static uint32_t rendered = 0;
    if (++rendered % 300 == 0) LogRendererIssues();
    {
      std::lock_guard<std::mutex> lock(g_queue_mutex);
      g_busy = false;
    }
    g_queue_cv.notify_all();
  }
}

// Image elargie : 16/9 du jeu (double de la projection de la camera, flottant des calculs de frustum), en memoire en
// lecture seule ouverte une fois.
void ApplyGameAspect(uint8_t* base) {
  static int32_t applied = 16;
  const int32_t aspect = REXCVAR_GET(native_gpu_aspect);
  if (aspect == applied) return;
  static const bool writable = [base] {
    const uintptr_t page = uintptr_t(sysconf(_SC_PAGESIZE));
    const uintptr_t start = reinterpret_cast<uintptr_t>(base + xd_profile::k_aspect_float) & ~(page - 1);
    return mprotect(reinterpret_cast<void*>(start), page, PROT_READ | PROT_WRITE) == 0;
  }();
  if (!writable) return;
  const float value = float(aspect) / 9.0f;
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  REX_STORE_U32(xd_profile::k_aspect_float, bits);
  const double projection = double(aspect) / 9.0;
  uint64_t projection_bits;
  std::memcpy(&projection_bits, &projection, 8);
  REX_STORE_U64(xd_profile::k_aspect_double, projection_bits);
  applied = aspect;
}

}  // namespace

// ---- crochets de la XDK D3D (d3d_hooks.generated.inc : ils appellent XdFrontOnCall apres l'appel d'origine) ----
#include "d3d_hooks.generated.inc"

void XdFrontOnCall(uint32_t function, const PPCContext& before, uint32_t result, uint8_t* base) {
  if (!g_renderer) return;
  const xenos::DeviceState state{base + kDevice};
  using namespace xd_profile;
  switch (function) {
    case k_BeginVertices:  // (device, prim, count, stride) -> pointeur de sommets
      g_inline_vertices = result;
      g_inline_primitive = before.r4.u32;
      g_inline_count = before.r5.u32;
      g_inline_stride = before.r6.u32;
      break;
    case k_DrawIndexedVertices:  // (device, prim, base vertex, start index, count)
    case k_DrawVertices:         // (device, prim, start vertex, count)
    case k_EndVertices: {        // (device)
      ng::DrawCall call;
      call.state = state;
      const uint32_t vs = REX_LOAD_U32(kDevice + xenos::device::kVertexShader);
      const uint32_t ps = REX_LOAD_U32(kDevice + xenos::device::kPixelShader);
      const ShaderObject vertex = FindShaderObject(vs);
      const ShaderObject pixel = FindShaderObject(ps);
      call.vs_key = vertex.key;
      call.ps_key = pixel.key;
      if (vs) {
        call.vs_physical = base + REX_LOAD_U32(vs + 32);
        call.vs_physical_size = vertex.physical_size;
      }
      if (function == k_EndVertices) {
        if (!g_inline_vertices) break;
        call.primitive = g_inline_primitive;
        call.count = g_inline_count;
        call.inline_vertices = base + g_inline_vertices;
        call.inline_size = g_inline_count * g_inline_stride;
        g_inline_vertices = 0;
      } else if (function == k_DrawIndexedVertices) {
        call.primitive = before.r4.u32;
        call.base_vertex = before.r5.u32;
        call.start = before.r6.u32;
        call.count = before.r7.u32;
        // Objet tampon d'indices : +0 bit 31 = indices 32 bits, +24 donnees, +28 taille.
        const uint32_t indices = REX_LOAD_U32(kDevice + xenos::device::kIndexBuffer);
        if (!indices) break;
        call.indices_32bit = REX_LOAD_U32(indices) >> 31;
        call.indices_address = g_memory->GetPhysicalAddress(REX_LOAD_U32(indices + 24));
        call.indices_size = REX_LOAD_U32(indices + 28);
      } else {
        call.primitive = before.r4.u32;
        call.start = before.r5.u32;
        call.count = before.r6.u32;
      }
      DeferredCall deferred{DeferredCall::Kind::kDraw, SnapshotState(base), call, {}, {}};
      std::vector<uint8_t>& arena = g_recording.arena;
      if (call.inline_vertices) {
        deferred.inline_vertices = arena.size();
        arena.insert(arena.end(), call.inline_vertices, call.inline_vertices + call.inline_size);
      }
      if (call.vs_physical) {
        deferred.vs_physical = arena.size();
        arena.insert(arena.end(), call.vs_physical, call.vs_physical + call.vs_physical_size);
      }
      g_recording.calls.push_back(deferred);
      break;
    }
    case k_ClearF: {  // (device, flags, rect, color)
      ng::ClearCall call;
      call.state = state;
      call.flags = before.r4.u32;
      call.has_rect = ReadRect(base, before.r5.u32, call.rect);
      ReadColor(base, before.r6.u32, call.color);
      call.depth = float(before.f1.f64);
      call.stencil = before.r7.u32;
      g_recording.calls.push_back({DeferredCall::Kind::kClear, SnapshotState(base), {}, call, {}});
      break;
    }
    case k_BeginTiling: {  // (device, flags, count, tile rects, clear color, ...) : rendu sans tuiles, un effacement de toutes
      if (!before.r6.u32 || !before.r5.u32) break;
      ng::ClearCall call;
      call.state = state;
      call.flags = 0x30;
      call.stencil = before.r8.u32;
      call.has_rect = true;
      call.rect[0] = call.rect[1] = INT32_MAX;
      call.rect[2] = call.rect[3] = INT32_MIN;
      for (uint32_t t = 0; t < before.r5.u32; ++t) {
        int32_t rect[4];
        ReadRect(base, before.r6.u32 + 16 * t, rect);
        for (int c = 0; c < 4; ++c) call.rect[c] = c < 2 ? std::min(call.rect[c], rect[c]) : std::max(call.rect[c], rect[c]);
      }
      if (before.r7.u32) {
        call.flags |= 0xF;
        ReadColor(base, before.r7.u32, call.color);
      }
      call.depth = float(before.f1.f64);
      g_recording.calls.push_back({DeferredCall::Kind::kClear, SnapshotState(base), {}, call, {}});
      break;
    }
    case k_Resolve: {  // (device, flags, source rect, texture, dest point, ...)
      ng::ResolveCall call;
      call.state = state;
      call.flags = before.r4.u32;
      call.has_rect = ReadRect(base, before.r5.u32, call.rect);
      if (before.r7.u32) {
        call.has_dest_point = true;
        call.dest_x = int32_t(REX_LOAD_U32(before.r7.u32));
        call.dest_y = int32_t(REX_LOAD_U32(before.r7.u32 + 4));
      }
      call.clear_depth = float(before.f1.f64);
      g_recording.calls.push_back({DeferredCall::Kind::kResolve, SnapshotState(base), {}, {}, call});
      if (!(call.flags & 4)) g_recording.front_buffer = state.Reg(0x2319);  // RB_COPY_DEST_BASE
      break;
    }
    default:
      break;
  }
}

// Registre des shaders : les objets sont crees sur place par ces deux aides (avant : le conteneur est intact apres).
REX_HOOK_RAW(sub_82CC1038) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t container = REX_LOAD_U32(ctx.r5.u32);
  __imp__sub_82CC1038(ctx, base);
  OnShaderCreate(base, object, container);
}
REX_HOOK_RAW(sub_82CC0EB0) {
  const uint32_t object = ctx.r3.u32;
  const uint32_t container = REX_LOAD_U32(ctx.r5.u32);
  __imp__sub_82CC0EB0(ctx, base);
  OnShaderCreate(base, object, container);
}

void XdFrontOnSwap(uint8_t* base) {
  if (!g_renderer) return;
  ApplyGameAspect(base);
  ngpu::NullGpuAttach(kDevice, base);
  const auto swap_start = std::chrono::steady_clock::now();
  static auto stats_start = swap_start;
  static uint32_t stats_frames = 0;
  ++stats_frames;
  const int32_t stats_seconds = REXCVAR_GET(native_gpu_stats_seconds);
  const auto elapsed = swap_start - stats_start;
  if (stats_seconds > 0 && elapsed >= std::chrono::seconds(stats_seconds)) {
    const double seconds = std::chrono::duration<double>(elapsed).count();
    REXLOG_INFO("native_gpu: {:.1f} FPS, {:.1f} ms/frame on the renderer thread", stats_frames / seconds,
                std::chrono::duration<double, std::milli>(g_thread_time).count() / stats_frames);
    g_renderer->SavePipelineCache();
    g_thread_time = {};
    stats_start = swap_start;
    stats_frames = 0;
  }
  const size_t recorded_calls = g_recording.calls.size();
  const size_t recorded_bytes = g_recording.arena.size();
  // Confie l'image des que le fil de rendu a pris la precedente (au plus une image en attente derriere celle en cours).
  {
    std::unique_lock<std::mutex> lock(g_queue_mutex);
    g_queue_cv.wait(lock, [] { return !g_queued.has_value(); });
    g_queued = std::move(g_recording);
  }
  g_queue_cv.notify_all();
  g_recording = Frame{};
  g_recording.calls.reserve(recorded_calls + recorded_calls / 4);
  g_recording.arena.reserve(recorded_bytes + recorded_bytes / 4);
}

void XdFrontStart(rex::Runtime* runtime) {
  g_memory = runtime->memory();
  // Cache de shaders compile avec le jeu : SPIR-V par cle de microcode
  native_gpu::StaticShaderProvider provider;
  if (!provider.Initialize()) {
    REXLOG_ERROR("native_gpu: shader cache unreadable");
    return;
  }
  g_watcher = std::make_unique<PhysicalWriteWatcher>(g_memory);
  g_core = std::make_unique<ng::Core>();
  ng::Profile profile;
  for (const uint64_t* k = xd_profile::kInterfaceVertexShaders; *k; ++k) profile.interface_vertex_shaders.push_back(*k);
  for (const uint64_t* k = xd_profile::kCornerVertexShaders; *k; ++k) profile.corner_vertex_shaders.push_back(*k);
  profile.scene_pitch_min = xd_profile::kScenePitchMin;
  profile.scene_pitch_max = xd_profile::kScenePitchMax;
  std::string error;
  if (!g_core->Initialize(g_memory->TranslatePhysical(0), FindShaderContainer, g_watcher.get(), profile, &error)) {
    REXLOG_ERROR("native_gpu: core initialization failed: {}", error);
    g_core.reset();
    return;
  }
  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
    std::vector<uint8_t> spirv;
    const uint64_t key = g_shaderCacheEntries[i].hash;
    if (provider.Find(key, spirv)) {
      g_core->shaders().AddBlob(key, std::move(spirv));
      g_known.insert(key);
    }
  }
  g_renderer = &g_core->renderer();
  {
    std::error_code ec;
    std::filesystem::create_directories(runtime->cache_root(), ec);
    g_renderer->LoadPipelineCache((runtime->cache_root() / "native_pipelines.bin").string());
  }
  // Rampe d'affichage : le D3D regle l'ecran par une rampe ecrite dans le tampon de commandes (non lu ici) ; approximation par une
  // puissance (docs/ngpu_core.md section 4).
  const double power = REXCVAR_GET(native_gpu_gamma) / 100.0;
  if (power > 0.0 && power != 1.0) {
    uint32_t ramp[256];
    for (int i = 0; i < 256; ++i) {
      const uint32_t v = uint32_t(std::lround(1023.0 * std::pow(i / 255.0, power)));
      ramp[i] = v << 20 | v << 10 | v;
    }
    g_renderer->SetGammaRamp(ramp);
  }
  std::thread(RendererThread).detach();
  REXLOG_INFO("native_gpu: ngpu_core on '{}', {} shaders, no GPU plugin", g_core->device_name(), g_known.size());
}
