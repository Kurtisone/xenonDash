// SPDX-License-Identifier: GPL-3.0-or-later
// XenonDash - facade de rendu natif GENERIQUE (recompilateur generique, games/_generic) sur le noyau ngpu_core.
//
// Generalisation de la facade de Skate (games/45410813/native/front.cpp, elle-meme reprise de la couche D3D native de Skate1Native,
// Kurtisone/Skate1Recompiled, GPL-3.0, commit 81805f51). Tout ce qui est propre a un jeu vient du profil genere par
// tools/recipe_build.py a partir de tools/d3d_match.py (d3d_profile.generated.h, d3d_hooks.generated.inc) : adresses des fonctions
// D3D, decalages. L'adresse du D3DDevice est capturee a l'execution : r3 d'une methode du peripherique retrouvee a haute confiance
// (seules celles-la sont crochetees), acceptee quand deux appels de suite donnent le meme pointeur plausible.
// La disposition de l'ombre du peripherique vient du profil (xenos::DeviceLayout, lue dans le code du jeu par d3d_match.py) : chaque
// appel enregistre un instantane CANONIQUE (ngpu_core/src/device_layout.h). Les jeux qui ecrivent leurs constantes de shaders
// directement dans le tampon de commandes (GpuBeginShaderConstantF4) sont suivis par un crochet sur cette fonction.
// Aucune heuristique de jeu : pas d'image elargie, pas de minicarte, pas de plage de pitch de scene (echelle de rendu 1).
// Reprise de la couche d'origine, adaptee a ngpu_core :
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
#include <cstdlib>
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
#include "device_layout.h"
#include "xd_project.h"
#include "ngpu.h"
#include "shader_cache.h"
#include "static_shader_provider.h"
#include "vk_backend.h"
#include "xd_front.h"

namespace ng = native_gpu;

REXCVAR_DEFINE_INT32(native_gpu_render_scale, 100, "Generic",
                     "Render and display the scene at this percentage of the console's resolution, 200 being 1440p")
    .range(100, 200)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(native_gpu_aspect, 16, "Generic",
                     "Picture width for a height of 9: 16 is the game's own, 21 and 32 show more on the sides")
    .range(16, 32)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(native_gpu_stats_seconds, 10, "Generic",
                     "Log frame rate and native renderer time every this many seconds (0 = off)");
REXCVAR_DEFINE_INT32(native_gpu_gamma, 116,
                     "Generic", "Display ramp approximated by a power, in hundredths (116 = 1.16, 100 = none)");
REXCVAR_DEFINE_STRING(native_gpu_dump_path, "", "Generic",
                      "Write the presented frame to this PPM file (see native_gpu_dump_frame)");
REXCVAR_DEFINE_INT32(native_gpu_dump_frame, 0, "Generic", "Frame number to write to native_gpu_dump_path (0 = none)");

namespace ngpu {
bool Verbose() { return std::getenv("DASH_NGPU_VERBOSE") != nullptr; }
bool Observe() { return false; }
bool Enabled() { return true; }
void NullGpuAttach(uint32_t dev, uint8_t* base);
}  // namespace ngpu

namespace {

uint32_t g_device = 0;   // D3DDevice invite : capture a l'execution (aucune adresse generique)
uint32_t g_device_candidate = 0;

// Disposition de l'ombre du peripherique du jeu (profil genere).
const xenos::DeviceLayout& Layout() {
  static const xenos::DeviceLayout layout = [] {
    xenos::DeviceLayout l;
    l.fetch_constants = xd_profile::kLayout_fetch_constants;
    l.vertex_constants = xd_profile::kLayout_vertex_constants;
    l.pixel_constants = xd_profile::kLayout_pixel_constants;
    l.vertex_bool_constants = xd_profile::kLayout_vertex_bool_constants;
    l.pixel_bool_constants = xd_profile::kLayout_pixel_bool_constants;
    for (int g = 0; g < 7; ++g) l.register_groups[g] = xd_profile::kLayout_register_groups[g];
    l.index_buffer = xd_profile::kLayout_index_buffer;
    l.pixel_shader = xd_profile::kLayout_pixel_shader;
    l.vertex_shader = xd_profile::kLayout_vertex_shader;
    return l;
  }();
  return layout;
}

bool Trace() {
  static const bool on = std::getenv("XD_NGPU_TRACE") != nullptr;
  return on;
}

// Un D3DDevice plausible : aligne, dans la memoire invitee adressable par le jeu, et dont le pointeur de retour du GPU (ecrit par
// l'initialisation de D3D) est nul ou lui-meme un pointeur invite.
bool PlausibleDevice(uint8_t* base, uint32_t device) {
  if (device < 0x10000 || device >= 0xF0000000u || (device & 3)) return false;
  const uint32_t fence = REX_LOAD_U32(device + xd_profile::kNull_fence_ptr);
  return fence == 0 || (fence >= 0x10000 && !(fence & 3));
}

// ---- constantes ecrites par le jeu dans le tampon de commandes (GpuBeginShaderConstantF4) ----
// La fonction reserve le paquet et rend l'adresse ou le jeu ecrit ensuite `count` vecteurs : ils sont relus au prochain appel D3D
// suivi (le jeu les a ecrits entre-temps) et recouvrent, registre par registre, les constantes de l'ombre dans l'instantane.
struct PendingConstants {
  uint32_t address, start, count;
  bool pixel;
};
std::vector<PendingConstants> g_pending_constants;
uint8_t g_gpu_constants[2][256 * 16];   // [0] sommets, [1] pixels : octets invites
uint32_t g_gpu_first[2] = {256, 256}, g_gpu_last[2] = {0, 0};   // plage de registres deja ecrits par cette voie
uint64_t g_gpu_constant_sets = 0;

void FlushPendingConstants(uint8_t* base) {
  for (const PendingConstants& p : g_pending_constants) {
    if (p.start >= 256 || !p.address) continue;
    const uint32_t count = std::min(p.count, 256 - p.start);
    std::memcpy(g_gpu_constants[p.pixel] + 16 * p.start, base + p.address, 16 * count);
    g_gpu_first[p.pixel] = std::min(g_gpu_first[p.pixel], p.start);
    g_gpu_last[p.pixel] = std::max(g_gpu_last[p.pixel], p.start + count);
  }
  g_pending_constants.clear();
}

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
std::unordered_set<uint64_t> g_missing_logged;

const uint8_t* FindShaderContainer(uint64_t key) {
  std::lock_guard<std::mutex> lock(g_shader_mutex);
  auto it = g_containers.find(key);
  return it == g_containers.end() ? nullptr : it->second.data();
}

// Microcode d'un shader absent du cache : conteneur ecrit dans le cache utilisateur (xd_profile::kMissingShaderDir) pour qu'une
// reconstruction l'ajoute (boucle « jouer -> reconstruire », docs/recompilateur_generique.md) ; le dessin correspondant est saute.
void DumpMissingShader(uint64_t hash, bool pixel, const uint8_t* data, size_t size) {
  if (!xd_profile::kMissingShaderDir[0]) return;
  std::error_code ec;
  std::filesystem::create_directories(xd_profile::kMissingShaderDir, ec);
  char name[64];
  std::snprintf(name, sizeof(name), "%016llX_%s.bin", static_cast<unsigned long long>(hash), pixel ? "ps" : "vs");
  const std::filesystem::path path = std::filesystem::path(xd_profile::kMissingShaderDir) / name;
  if (std::filesystem::exists(path, ec)) return;
  if (FILE* f = std::fopen(path.string().c_str(), "wb")) {
    std::fwrite(data, 1, size, f);
    std::fclose(f);
  }
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
  ++g_misses;
  if (g_missing_logged.insert(hash).second) {
    REXLOG_WARN("native_gpu: {} shader {:016X} not in the shader cache (object {:08X}) : draw skipped, microcode written to {}",
                pixel ? "pixel" : "vertex", hash, object, xd_profile::kMissingShaderDir);
    DumpMissingShader(hash, pixel, base + container, size_t(virtual_size) + REX_LOAD_U32(container + 8));
  }
}

ShaderObject FindShaderObject(uint32_t object) {
  if (!object) return {};
  std::lock_guard<std::mutex> lock(g_shader_mutex);
  auto it = g_objects.find(object);
  return it == g_objects.end() ? ShaderObject{} : it->second;
}

// ---- enregistrement de l'image ----
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
  arena.resize(offset + xenos::kCanonicalStateSize);
  uint8_t* state = arena.data() + offset;
  xenos::Canonicalize(Layout(), base + g_device, state);
  for (int pixel = 0; pixel < 2; ++pixel) {
    if (g_gpu_first[pixel] >= g_gpu_last[pixel]) continue;
    const uint32_t block = pixel ? xenos::device::kPixelConstants : xenos::device::kVertexConstants;
    std::memcpy(state + block + 16 * g_gpu_first[pixel], g_gpu_constants[pixel] + 16 * g_gpu_first[pixel],
                16 * (g_gpu_last[pixel] - g_gpu_first[pixel]));
  }
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

}  // namespace

// ---- crochets de la XDK D3D (d3d_hooks.generated.inc : ils appellent XdFrontOnCall apres l'appel d'origine) ----
#include "d3d_hooks.generated.inc"

void XdFrontOnShaderCreate(uint8_t* base, uint32_t object, uint32_t container) { OnShaderCreate(base, object, container); }

void XdFrontOnGpuConstants(uint8_t* base, uint32_t pixel, uint32_t start, uint32_t count, uint32_t address) {
  FlushPendingConstants(base);   // le bloc precedent est ecrit : le jeu ne reserve le suivant qu'apres
  if (!address || !count) return;
  g_pending_constants.push_back({address, start, count, pixel != 0});
  ++g_gpu_constant_sets;
}

namespace {
// Dessin a sommets en ligne ouvert par BeginVertices : D3D emet le paquet et l'etat des BeginVertices, le jeu ecrit les sommets
// ensuite. Sans crochet EndVertices (fonction de trois instructions, fondue dans le code des XDK tardives), le dessin est clos au
// prochain appel D3D suivi.
struct InlineDraw {
  bool open = false;
  size_t state = 0;
  uint32_t vertices = 0, primitive = 0, count = 0, stride = 0, vs = 0, ps = 0;
} g_inline;

void RecordDraw(uint8_t* base, ng::DrawCall call, size_t state_offset, uint32_t vs, uint32_t ps) {
  static uint64_t unbound = 0;
  if (!vs || !ps) {   // aucun shader lie dans l'ombre : le jeu les charge par une autre voie que Set{Vertex,Pixel}Shader
    if (++unbound == 1 || unbound % 5000 == 0) {
      REXLOG_WARN("native_gpu: draw without a bound shader in the device shadow (vs {:08X}, ps {:08X}) : skipped ({}x)", vs, ps,
                  unbound);
    }
    return;
  }
  const ShaderObject vertex = FindShaderObject(vs);
  const ShaderObject pixel = FindShaderObject(ps);
  call.vs_key = vertex.key;
  call.ps_key = pixel.key;
  call.vs_physical = base + REX_LOAD_U32(vs + xd_profile::kShader_physical_ptr);
  call.vs_physical_size = vertex.physical_size;
  DeferredCall deferred{DeferredCall::Kind::kDraw, state_offset, call, {}, {}};
  std::vector<uint8_t>& arena = g_recording.arena;
  if (call.inline_vertices) {
    deferred.inline_vertices = arena.size();
    arena.insert(arena.end(), call.inline_vertices, call.inline_vertices + call.inline_size);
  }
  if (call.vs_physical && call.vs_physical_size) {
    deferred.vs_physical = arena.size();
    arena.insert(arena.end(), call.vs_physical, call.vs_physical + call.vs_physical_size);
  }
  g_recording.calls.push_back(deferred);
}

void CloseInlineDraw(uint8_t* base) {
  if (!g_inline.open) return;
  g_inline.open = false;
  if (!g_inline.vertices) return;
  ng::DrawCall call;
  call.primitive = g_inline.primitive;
  call.count = g_inline.count;
  call.inline_vertices = base + g_inline.vertices;
  call.inline_size = g_inline.count * g_inline.stride;
  RecordDraw(base, call, g_inline.state, g_inline.vs, g_inline.ps);
}
}  // namespace

void XdFrontOnCall(uint32_t function, const PPCContext& before, uint32_t result, uint8_t* base) {
  using namespace xd_profile;
  const uint32_t device = before.r3.u32;   // seules des methodes du peripherique retrouvees a haute confiance sont crochetees
  if (device != g_device) {
    if (PlausibleDevice(base, device) && device == g_device_candidate) {
      REXLOG_INFO("native_gpu: D3DDevice captured at {:08X}{}", device, g_device ? " (replaces the previous one)" : "");
      g_device = device;
    } else {
      g_device_candidate = device;
    }
  }
  if (!g_renderer || !g_device || device != g_device) return;
  FlushPendingConstants(base);
  if (function != k_EndVertices) CloseInlineDraw(base);
  const uint32_t vs = REX_LOAD_U32(g_device + Layout().vertex_shader);
  const uint32_t ps = REX_LOAD_U32(g_device + Layout().pixel_shader);
  if (Trace()) {
    static uint32_t traced = 0;
    if (traced++ < 400) {
      REXLOG_INFO("native_gpu trace: call {} r4 {:08X} r5 {:08X} r6 {:08X} r7 {:08X} result {:08X} vs {:08X} ps {:08X} constant sets {}",
                  function, before.r4.u32, before.r5.u32, before.r6.u32, before.r7.u32, result, vs, ps, g_gpu_constant_sets);
    }
  }
  switch (function) {
    case k_BeginVertices:  // (device, prim, count, stride) -> pointeur de sommets
      g_inline = {true, SnapshotState(base), result, before.r4.u32, before.r5.u32, before.r6.u32, vs, ps};
      break;
    case k_EndVertices:  // (device)
      CloseInlineDraw(base);
      break;
    case k_DrawIndexedVertices:  // (device, prim, base vertex, start index, count)
    case k_DrawVertices: {       // (device, prim, start vertex, count)
      ng::DrawCall call;
      if (function == k_DrawIndexedVertices) {
        call.primitive = before.r4.u32;
        call.base_vertex = before.r5.u32;
        call.start = before.r6.u32;
        call.count = before.r7.u32;
        // Objet tampon d'indices : +0 bit 31 = indices 32 bits, +24 donnees, +28 taille.
        const uint32_t indices = REX_LOAD_U32(g_device + Layout().index_buffer);
        if (!indices) break;
        call.indices_32bit = REX_LOAD_U32(indices) >> 31;
        call.indices_address = g_memory->GetPhysicalAddress(REX_LOAD_U32(indices + 24));
        call.indices_size = REX_LOAD_U32(indices + 28);
      } else {
        call.primitive = before.r4.u32;
        call.start = before.r5.u32;
        call.count = before.r6.u32;
      }
      RecordDraw(base, call, SnapshotState(base), vs, ps);
      break;
    }
    case k_Clear: {  // (device, flags, rect, color)
      ng::ClearCall call;
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
      call.flags = before.r4.u32;
      call.has_rect = ReadRect(base, before.r5.u32, call.rect);
      if (before.r7.u32) {
        call.has_dest_point = true;
        call.dest_x = int32_t(REX_LOAD_U32(before.r7.u32));
        call.dest_y = int32_t(REX_LOAD_U32(before.r7.u32 + 4));
      }
      call.clear_depth = float(before.f1.f64);
      const size_t snapshot = SnapshotState(base);
      g_recording.calls.push_back({DeferredCall::Kind::kResolve, snapshot, {}, {}, call});
      if (!(call.flags & 4)) {
        g_recording.front_buffer = xenos::DeviceState{g_recording.arena.data() + snapshot}.Reg(0x2319);  // RB_COPY_DEST_BASE
      }
      break;
    }
    default:
      break;
  }
}

void XdFrontOnSwap(uint8_t* base) {
  if (!g_renderer) return;
  FlushPendingConstants(base);
  CloseInlineDraw(base);
  if (Trace()) {
    static uint32_t swaps = 0;
    if (swaps++ < 600) {
      REXLOG_INFO("native_gpu trace: swap {} : {} calls recorded, device {:08X}, front buffer {:08X}", swaps, g_recording.calls.size(),
                  g_device, g_recording.front_buffer);
    }
  }
  if (g_device) ngpu::NullGpuAttach(g_device, base);
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
