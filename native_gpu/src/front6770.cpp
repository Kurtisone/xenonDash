// SPDX-License-Identifier: GPL-3.0-or-later
// native_gpu : facade D3D du Dashboard 6770 sur le noyau de rendu commun ngpu_core (P8, docs/ngpu_core.md).
//
// Meme cartographie D3D que hooks_draw.cpp/hooks_frame.cpp (docs/rendu_natif.md §11), mais au lieu de dessiner par le
// backend Vulkan maison, chaque appel D3D suivi est traduit en appels du noyau (Renderer::Draw / Clear / Resolve /
// EndFrame). Le noyau lit l'etat Xenos dans une « ombre de peripherique » (disposition du XDK 5632, xenos_decode.h) :
// le D3D du 6770 n'a pas cette ombre (etat suivi par appel de fonction), la facade la SYNTHETISE a chaque dessin :
//   - registres d'etat : copie du cache de registres du peripherique 6770 (memes offsets : +10368.. groupes 0x2000-0x2380),
//   - constantes de sommets/pixels : celles passees a Set*ShaderConstantF (octets grand-boutistes tels quels),
//   - fetch constants de textures : les 6 mots a +28 de l'objet D3DTexture (adresse corrigee pour la region E),
//   - microcode du vertex shader : copie de la partie physique du conteneur dont les vfetch sont recrits d'apres la
//     declaration de sommets (le microcode du conteneur a des vfetch au format 0 : D3D les complete a l'execution).
// Les primitives que le noyau ne sait pas (rect list 8, fan 5) sont developpees en listes de triangles ici.
//   9212EE10/9212ED00  CreateVertexShader/CreatePixelShader   9212EB48/9212EA00  CreateVertexDeclaration/SetVertexDeclaration
//   9212E7E8/9212E4E8  SetVertexShader/SetPixelShader         92123F20  SetTexture
//   9212E2D8/9212E3C0  Set{Vertex,Pixel}ShaderConstantF       925CCB68  DrawVerticesUP        925CD0E0  DrawIndexedVerticesUP
//   925CC6B8/925CCB58  BeginVertices/EndVertices              92128AF0  Clear                 92121EE8  Swap (hooks_frame.cpp)

#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/system/kernel_state.h>

#include "device.h"
#include "ngpu.h"
#include "vk_backend.h"

namespace ngpu {
uint64_t FrameCounter();
}

namespace {

using namespace ngpu;
namespace ng = native_gpu;

constexpr uint32_t kShadowSize = 0x3400;
constexpr uint32_t kRegsFirst = 10368, kRegsEnd = 10960;  // groupes de registres 0x2000..0x239F du cache du peripherique
constexpr uint32_t kCopyDestBase = 0x2319 - 0x2300, kCopyDestPitch = 0x231A - 0x2300;
constexpr uint32_t kFrontBufferAddress = 0x1F000000;  // adresse fictive du tampon d'image (cle des resolves)

uint8_t* GBase() { return rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(0); }

// ---- surveillance des ecritures de la memoire physique (reprise de live_renderer.cpp de Skate1Native) ----
class PhysicalWriteWatcher : public ng::WriteWatcher {
 public:
  explicit PhysicalWriteWatcher(rex::memory::Memory* memory) : memory_(memory) {
    handle_ = memory_->RegisterPhysicalMemoryInvalidationCallback(&OnWrite, this);
  }
  ~PhysicalWriteWatcher() override { memory_->UnregisterPhysicalMemoryInvalidationCallback(handle_); }
  uint32_t Watch(uint32_t address, uint32_t size) override {
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

struct ShaderInfo {
  bool pixel = false;
  uint32_t container = 0;
  uint64_t key = 0;
  struct Element { uint32_t usage, usage_index, address; };
  std::vector<Element> elements;  // vertex shader : elements d'entree dans l'ordre du conteneur
  uint32_t virtual_size = 0, physical_size = 0, microcode_offset = 0;
};

struct DeclElem { uint32_t usage, uidx, offset, type; };
struct Decl {
  std::vector<DeclElem> elems;
  uint64_t signature = 0;
};

struct Front {
  std::mutex mutex;
  bool init_tried = false;
  std::unique_ptr<ng::Core> core;
  std::unique_ptr<PhysicalWriteWatcher> watcher;
  bool in_frame = false;
  uint64_t frames = 0;

  uint32_t vs = 0, ps = 0, decl = 0;
  uint32_t tex[16] = {};
  alignas(16) uint8_t vsc[4096] = {}, psc[4096] = {};  // octets grand-boutistes (256 float4)
  std::unordered_map<uint32_t, ShaderInfo> objects;               // objet D3D -> infos
  std::unordered_map<uint64_t, std::vector<uint8_t>> containers;  // cle -> conteneur copie (virtuel + physique)
  std::unordered_map<uint32_t, Decl> decls;
  struct VsKey {
    uint64_t key, decl;
    uint32_t stride;
    bool operator<(const VsKey& o) const { return std::tie(key, decl, stride) < std::tie(o.key, o.decl, o.stride); }
  };
  std::map<VsKey, std::vector<uint8_t>> patched;  // partie physique des VS recrite d'apres la declaration
  alignas(16) uint8_t shadow[kShadowSize] = {};
  std::vector<uint8_t> scratch, scratch_idx;
  uint64_t draws = 0, skipped = 0;
} g;

std::string ShaderDir() {
  const char* e = std::getenv("DASH_NGPU_SHADERS");
  return e && *e ? e : "work/shaders_out";
}

bool FileExists(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
}

// Appelee avec g.mutex tenu.
bool EnsureCore() {
  if (g.core) return true;
  if (g.init_tried) return false;
  g.init_tried = true;
  auto* memory = rex::system::kernel_memory();
  g.watcher = std::make_unique<PhysicalWriteWatcher>(memory);
  auto core = std::make_unique<ng::Core>();
  ng::Profile profile;  // Dashboard : aucune echelle de scene ni liste d'interface (pas d'image elargie)
  profile.scene_pitch_min = 1u << 30;
  profile.scene_pitch_max = 0;
  std::string error;
  if (!core->Initialize(
          memory->TranslatePhysical(0),
          [](uint64_t key) -> const uint8_t* {
            auto it = g.containers.find(key);
            return it == g.containers.end() ? nullptr : it->second.data();
          },
          g.watcher.get(), profile, &error)) {
    NGPU_ERR("noyau de rendu : %s", error.c_str());
    return false;
  }
  NGPU_LOG("noyau ngpu_core sur '%s'", core->device_name().c_str());
  std::fprintf(stderr, "[ngpu] noyau ngpu_core sur '%s'\n", core->device_name().c_str());
  // Rampe d'affichage : le D3D du Dashboard regle l'ecran en gamma BT.709 (VdGetCurrentDisplayGamma, type 2) et ecrit une rampe PWL
  // dans les registres DC_LUT, que l'oracle applique a la presentation. Ces ecritures partent dans le tampon de commandes (non lu
  // ici) : approximation par une puissance (mesuree contre l'oracle : sortie = entree^1.16 entre 20 % et 95 %). DASH_NGPU_GAMMA=<p>
  // change l'exposant, 1 = pas de rampe.
  {
    double power = 1.16;
    if (const char* e = std::getenv("DASH_NGPU_GAMMA")) power = std::atof(e);
    if (power > 0.0 && power != 1.0) {
      uint32_t ramp[256];
      for (int i = 0; i < 256; ++i) {
        const uint32_t v = uint32_t(std::lround(1023.0 * std::pow(i / 255.0, power)));
        ramp[i] = v << 20 | v << 10 | v;
      }
      core->renderer().SetGammaRamp(ramp);
    }
  }
  g.core = std::move(core);
  return true;
}

void BeginFrameIfNeeded() {
  if (!g.in_frame) {
    g.core->renderer().BeginFrame();
    g.in_frame = true;
  }
}

// ---- construction de l'ombre de peripherique canonique ----
// Le D3D du 6770 (XDK 2.0.5526) a la MEME disposition d'ombre que celui de Skate (5632) pour ce qui sert au noyau : fetch constants a
// +0x480 (fusionnees avec l'etat d'echantillonnage par SetTexture/SetSamplerState), constantes de sommets a +0x780 et de pixels a
// +0x1780 (vu dans le code de SetTexture 92123F20 et Set*ShaderConstantF), registres a +10368... L'ombre est donc un simple
// instantane du peripherique ; seules les adresses de textures de la region E sont corrigees (+0x1000, docs/rendu_natif.md §12.1).
void BuildShadow(const uint8_t* base, uint32_t dev) {
  std::memcpy(g.shadow, base + dev, kShadowSize);
  static const bool point = std::getenv("DASH_NGPU_POINT") != nullptr;
  for (uint32_t slot = 0; slot < 16; ++slot) {
    uint8_t* fetch = g.shadow + xenos::device::kFetchConstants + 24 * slot;
    uint32_t d[6];
    for (int i = 0; i < 6; ++i) d[i] = Be32(fetch + 4 * i);
    if ((d[0] & 3) != 2) continue;  // pas une texture
    const auto fix = [](uint32_t word) {
      const uint32_t addr = word & 0xFFFFF000;
      return addr >= 0xE0000000u ? (word & 0xFFF) | (addr + 0x1000) : word;
    };
    d[1] = fix(d[1]);
    if (d[5] & 0xFFFFF000) d[5] = fix(d[5]);
    if (point) d[3] &= ~((3u << 19) | (3u << 21));
    for (int i = 0; i < 6; ++i) PutBe32(fetch + 4 * i, d[i]);
  }
}

// ---- shaders ----
void OnCreateShader(const uint8_t* base, uint32_t object, uint32_t container, bool pixel) {
  if (!object || !container) return;
  const uint8_t* c = base + container;
  const uint32_t magic = Be32(c);
  if ((magic & 0xFFFFFFFE) != 0x102A1100) return;
  const uint32_t virtual_size = Be32(c + 4), physical_size = Be32(c + 8);
  const uint8_t* shader = c + Be32(c + 0x18);
  const uint32_t microcode_offset = Be32(shader), size = Be32(shader + 4);
  const uint64_t key = XXH3_64bits(c + virtual_size + microcode_offset, size);
  std::lock_guard<std::mutex> lock(g.mutex);
  ShaderInfo info;
  info.pixel = pixel;
  info.container = container;
  info.key = key;
  info.virtual_size = virtual_size;
  info.physical_size = physical_size;
  info.microcode_offset = microcode_offset;
  if (!pixel) {
    const uint32_t f18 = Be32(shader + 0x18), count = Be32(shader + 0x1C);
    for (uint32_t i = 0; i < count && i < 16; ++i) {
      const uint32_t v = Be32(shader + 0x24 + 4 * (f18 + i));
      info.elements.push_back({(v >> 12) & 0xF, (v >> 16) & 0xF, v & 0xFFF});
    }
  }
  g.objects[object] = info;
  if (!g.containers.count(key)) {
    g.containers.emplace(key, std::vector<uint8_t>(c, c + virtual_size + physical_size));
    // SPIR-V de XenosRecomp : un fichier par conteneur (adresse du conteneur dans dash.xex), retrouve par la cle de microcode.
    char name[96];
    std::string path;
    for (const char* prefix : {"ui", "video"}) {
      std::snprintf(name, sizeof name, "/%s_%08X_%s.spv", prefix, container, pixel ? "PS" : "VS");
      path = ShaderDir() + name;
      if (FileExists(path)) break;
      path.clear();
    }
    if (EnsureCore()) {
      if (!path.empty()) g.core->shaders().AddFile(key, path);
      else NGPU_ERR("SPIR-V introuvable pour le shader %08X (%s) dans %s", container, pixel ? "PS" : "VS", ShaderDir().c_str());
    }
  }
}

size_t ElementBytes(uint32_t format) {
  switch (format) {
    case 36: case 6: case 25: case 31: return 4;
    case 37: case 26: case 32: return 8;
    case 57: return 12;
    case 38: return 16;
    default: return 4;
  }
}

// Partie physique du VS dont les vfetch sont recrits d'apres la declaration (slot 0, format, decalage, pas).
const std::vector<uint8_t>* PatchedVertexMicrocode(const ShaderInfo& vs, const Decl& decl, uint32_t stride) {
  Front::VsKey key{vs.key, decl.signature, stride};
  auto it = g.patched.find(key);
  if (it != g.patched.end()) return &it->second;
  const auto& container = g.containers[vs.key];
  std::vector<uint8_t> physical(container.begin() + vs.virtual_size, container.end());
  for (const ShaderInfo::Element& element : vs.elements) {
    uint8_t* instruction = physical.data() + vs.microcode_offset + element.address * 12;
    if (instruction + 12 > physical.data() + physical.size()) continue;
    uint32_t w0 = Be32(instruction), w1 = Be32(instruction + 4), w2 = Be32(instruction + 8);
    const DeclElem* found = nullptr;
    for (const DeclElem& d : decl.elems) {
      if (d.usage == element.usage && d.uidx == element.usage_index) {
        found = &d;
        break;
      }
    }
    w0 &= ~((0x1Fu << 20) | (3u << 25));  // un seul flux : fetch slot 0
    if (found) {
      const uint32_t format = found->type & 0x3F;
      w1 &= ~((1u << 12) | (1u << 13) | (0x3Fu << 16) | (0x3Fu << 24) | (1u << 30));  // non signe, fraction, pas de mini ni exp_adjust
      if (format == 25 || format == 26) w1 |= 1u << 12;                                // 16_16(_16_16) : signes
      w1 |= format << 16;
      w2 = (w2 & 0x80000000u) | (stride / 4 & 0xFF) | ((found->offset / 4) << 8);
    } else {
      w1 = (w1 & ~0xFFFu) | 0x924;  // element absent de la declaration : constantes seulement (le noyau le laisse non lie)
    }
    PutBe32(instruction, w0);
    PutBe32(instruction + 4, w1);
    PutBe32(instruction + 8, w2);
  }
  return &g.patched.emplace(key, std::move(physical)).first->second;
}

// ---- dessin ----
// Developpe rect list (8) et fan (5), et les listes de quads indexees, en liste de triangles : sommets recopies, le
// quatrieme sommet du rectangle = b + c - a. Les elements viennent de la declaration.
void ExpandPrimitive(uint32_t prim, uint32_t count, const uint8_t* data, uint32_t stride, const std::vector<uint32_t>* indices,
                     const Decl& decl, std::vector<uint8_t>& out, uint32_t& out_count) {
  auto vertex = [&](uint32_t i) { return data + size_t(indices ? (*indices)[i] : i) * stride; };
  out.clear();
  out_count = 0;
  auto emit = [&](const uint8_t* v) {
    out.insert(out.end(), v, v + stride);
    ++out_count;
  };
  std::vector<uint8_t> fourth(stride);
  auto combine = [&](const uint8_t* a, const uint8_t* b, const uint8_t* c) {
    std::memcpy(fourth.data(), c, stride);
    for (const DeclElem& e : decl.elems) {
      const uint32_t format = e.type & 0x3F;
      if (e.offset + ElementBytes(format) > stride) continue;
      uint8_t* d = fourth.data() + e.offset;
      switch (format) {
        case 36: case 37: case 57: case 38: {
          const uint32_t n = format == 36 ? 1 : format == 37 ? 2 : format == 57 ? 3 : 4;
          for (uint32_t k = 0; k < n; ++k) {
            float r = BeF32(b + e.offset + 4 * k) + BeF32(c + e.offset + 4 * k) - BeF32(a + e.offset + 4 * k), tmp = r;
            uint32_t bits;
            std::memcpy(&bits, &tmp, 4);
            PutBe32(d + 4 * k, bits);
          }
          break;
        }
        case 6:
          for (int k = 0; k < 4; ++k) d[k] = uint8_t(std::clamp(int(b[e.offset + k]) + c[e.offset + k] - a[e.offset + k], 0, 255));
          break;
        case 25: case 26: {
          const uint32_t n = format == 25 ? 2 : 4;
          for (uint32_t k = 0; k < n; ++k) {
            const int v = int(int16_t(Be16(b + e.offset + 2 * k))) + int16_t(Be16(c + e.offset + 2 * k)) - int16_t(Be16(a + e.offset + 2 * k));
            const uint16_t u = uint16_t(int16_t(std::clamp(v, -32768, 32767)));
            d[2 * k] = u >> 8;
            d[2 * k + 1] = u & 0xFF;
          }
          break;
        }
        default:
          std::memcpy(d, a + e.offset, ElementBytes(format));
          break;
      }
    }
    return fourth.data();
  };
  if (prim == 8) {
    for (uint32_t i = 0; i + 2 < count; i += 3) {
      const uint8_t *a = vertex(i), *b = vertex(i + 1), *c = vertex(i + 2);
      const uint8_t* d = combine(a, b, c);
      std::vector<uint8_t> dd(d, d + stride);
      emit(a); emit(b); emit(c); emit(c); emit(b); emit(dd.data());
    }
  } else if (prim == 5) {
    for (uint32_t i = 1; i + 1 < count; ++i) {
      emit(vertex(0)); emit(vertex(i)); emit(vertex(i + 1));
    }
  } else if (prim == 13) {
    for (uint32_t i = 0; i + 3 < count; i += 4) {
      emit(vertex(i)); emit(vertex(i + 1)); emit(vertex(i + 2)); emit(vertex(i)); emit(vertex(i + 2)); emit(vertex(i + 3));
    }
  }
}

bool DoDraw(uint8_t* base, uint32_t dev, uint32_t prim, uint32_t count, uint32_t data, uint32_t stride, const std::vector<uint32_t>* indices,
            bool indices32) {
  std::lock_guard<std::mutex> lock(g.mutex);
  g.draws++;
  if (std::getenv("DASH_NGPU_NODRAW")) return true;
  auto skip = [&](const char* why) {
    g.skipped++;
    NGPU_LOG("dessin %llu ignore : %s", (unsigned long long)g.draws, why);
    return true;
  };
  if (!EnsureCore()) return true;
  {  // DASH_NGPU_DRAWRANGE="a:b" : ne dessine que les dessins a..b de chaque image (bissection des artefacts)
    static uint64_t frame = ~0ull, k = 0;
    if (frame != g.frames) { frame = g.frames; k = 0; }
    ++k;
    static const char* range = std::getenv("DASH_NGPU_DRAWRANGE");
    if (range) {
      unsigned a = 0, b = ~0u;
      std::sscanf(range, "%u:%u", &a, &b);
      if (k < a || k > b) return true;
    }
  }
  auto vo = g.objects.find(g.vs), po = g.objects.find(g.ps);
  if (vo == g.objects.end() || po == g.objects.end()) return skip("shader inconnu");
  auto dit = g.decls.find(g.decl);
  if (dit == g.decls.end()) return skip("declaration de sommets inconnue");
  if (!count || !stride || stride > 1020) return true;
  const ShaderInfo& vs = vo->second;
  const ShaderInfo& ps = po->second;
  BeginFrameIfNeeded();

  const uint8_t* vertices = base + data;
  ng::DrawCall call;
  BuildShadow(base, dev);
  if (const char* rv = std::getenv("DASH_NGPU_REGVARY"); rv && g.frames == uint64_t(std::atoll(rv))) {  // diagnostic : registres qui varient d'un dessin a l'autre
    static uint8_t last[kShadowSize];
    static bool have = false;
    static std::map<uint32_t, int> changed;
    if (have) for (uint32_t o = kRegsFirst; o < kRegsEnd; o += 4) if (std::memcmp(last + o, g.shadow + o, 4) && changed[o]++ == 0)
      std::fprintf(stderr, "[regvary] dessin %llu : +%u (reg %#x) %08X -> %08X\n", (unsigned long long)g.draws, o, 0, Be32(last + o), Be32(g.shadow + o));
    std::memcpy(last, g.shadow, kShadowSize);
    have = true;
  }
  call.state.data = g.shadow;
  call.vs_key = vs.key;
  call.ps_key = ps.key;
  const std::vector<uint8_t>* physical = PatchedVertexMicrocode(vs, dit->second, stride);
  call.vs_physical = physical->data();
  call.vs_physical_size = uint32_t(physical->size());

  uint32_t max_index = count;
  if (indices) {
    max_index = 0;
    for (uint32_t i : *indices) max_index = std::max(max_index, i + 1);
  }
  if (prim == 5 || prim == 8 || (prim == 13 && indices)) {
    uint32_t out_count = 0;
    ExpandPrimitive(prim, count, vertices, stride, indices, dit->second, g.scratch, out_count);
    if (!out_count) return true;
    call.primitive = 4;
    call.count = out_count;
    call.inline_vertices = g.scratch.data();
    call.inline_size = uint32_t(g.scratch.size());
  } else {
    call.primitive = prim;
    call.count = count;
    call.inline_vertices = vertices;
    call.inline_size = max_index * stride;
    if (indices) {
      g.scratch_idx.resize(indices->size() * (indices32 ? 4 : 2));
      for (size_t i = 0; i < indices->size(); ++i) {
        if (indices32) PutBe32(&g.scratch_idx[4 * i], (*indices)[i]);
        else { g.scratch_idx[2 * i] = uint8_t((*indices)[i] >> 8); g.scratch_idx[2 * i + 1] = uint8_t((*indices)[i]); }
      }
      call.inline_indices = g.scratch_idx.data();
      call.indices_size = uint32_t(g.scratch_idx.size());
      call.indices_32bit = indices32;
    }
  }
  if (const char* di = std::getenv("DASH_NGPU_DRAWINFO"); di && g.frames == uint64_t(std::atoll(di))) {  // diagnostic : etat du dessin
    const xenos::DeviceState& st = call.state;
    std::fprintf(stderr, "[di %llu] prim=%u n=%u stride=%u vs=%016llX ps=%016llX surf=%08X ci=%08X di=%08X blend=%08X cc=%08X dc=%08X mode=%08X vte=%08X mask=%X vp=%g,%g,%g,%g,%g,%g tex0=%08X\n",
                 (unsigned long long)g.draws, prim, count, stride, (unsigned long long)vs.key, (unsigned long long)ps.key, st.Reg(0x2000), st.Reg(0x2001), st.Reg(0x2002), st.Reg(0x2201),
                 st.Reg(0x2202), st.Reg(0x2200), st.Reg(0x2205), st.Reg(0x2206), st.Reg(0x2104), st.RegF(0x210F), st.RegF(0x2110), st.RegF(0x2111), st.RegF(0x2112), st.RegF(0x2113), st.RegF(0x2114), g.tex[0]);
    if (g.tex[0]) {
      const xenos::TextureFetch t = xenos::DecodeTextureFetch(st, 0);
      std::fprintf(stderr, "     tex0: fmt %u endian %u %s %ux%u pitch %u dim %u swz %03X mips %u..%u mipaddr %08X addr %08X filt %u/%u/%u clamp %u,%u signed %d\n", t.format, t.endian, t.tiled ? "tiled" : "linear", t.width, t.height, t.pitch, t.dimension,
                   t.swizzle, t.mip_min_level, t.mip_max_level, t.mip_address, t.address, t.mag_filter, t.min_filter, t.mip_filter, t.clamp[0], t.clamp[1], int(t.is_signed));
    }
    for (const DeclElem& e : dit->second.elems) {
      if (e.usage != 0 || e.uidx != 0) continue;
      std::fprintf(stderr, "     pos(fmt %u):", e.type & 0x3F);
      for (uint32_t v = 0; v < count && v < 6; ++v) std::fprintf(stderr, " (%g,%g)", BeF32(vertices + (indices ? (*indices)[v] : v) * size_t(stride) + e.offset), BeF32(vertices + (indices ? (*indices)[v] : v) * size_t(stride) + e.offset + 4));
      std::fprintf(stderr, "  c0..3:");
      for (int k = 0; k < 8; ++k) std::fprintf(stderr, " %g", BeF32(g.vsc + 4 * k));
      std::fputc('\n', stderr);
    }
  }
  g.core->renderer().Draw(call);
  return true;
}

}  // namespace

// ----------------------------------------------------------------------------------------------------------
// Creation et selection des shaders / declarations
// ----------------------------------------------------------------------------------------------------------
NGPU_HOOK(9212EE10) {
  const uint32_t container = ctx.r3.u32;
  NGPU_ORIG(9212EE10);
  OnCreateShader(base, ctx.r3.u32, container, false);
  return true;
}
NGPU_HOOK(9212ED00) {
  const uint32_t container = ctx.r3.u32;
  NGPU_ORIG(9212ED00);
  OnCreateShader(base, ctx.r3.u32, container, true);
  return true;
}
// D3DVERTEXELEMENT9 de 12 octets {u16 flux, u16 decalage, u32 type (format GPU : 6 bits bas), u8 methode, u8 usage, u8 indice, u8 -}.
NGPU_HOOK(9212EB48) {
  Decl decl;
  const uint32_t p = ctx.r3.u32;
  for (int i = 0; i < 16; i++) {
    const uint8_t* e = base + p + 12 * i;
    if (Be16(e) == 0xFF) break;
    decl.elems.push_back({e[9], e[10], Be16(e + 2), Be32(e + 4)});
  }
  decl.signature = XXH3_64bits(decl.elems.data(), decl.elems.size() * sizeof(DeclElem));
  NGPU_ORIG(9212EB48);
  if (ctx.r3.u32) {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.decls[ctx.r3.u32] = std::move(decl);
  }
  return true;
}
NGPU_HOOK(9212EA00) { g.decl = ctx.r4.u32; return false; }
NGPU_HOOK(9212E4E8) { g.ps = ctx.r4.u32; return false; }
NGPU_HOOK(9212E7E8) { g.vs = ctx.r4.u32; return false; }
NGPU_HOOK(92123F20) {
  if (ctx.r4.u32 < 16) g.tex[ctx.r4.u32] = ctx.r5.u32;
  return false;
}
NGPU_HOOK(9212E2D8) {
  const uint32_t r = ctx.r4.u32, n = ctx.r6.u32;
  if (r < 256 && n <= 256 - r) std::memcpy(g.vsc + r * 16, base + ctx.r5.u32, n * 16);
  return false;
}
NGPU_HOOK(9212E3C0) {
  const uint32_t r = ctx.r4.u32, n = ctx.r6.u32;
  if (r < 256 && n <= 256 - r) std::memcpy(g.psc + r * 16, base + ctx.r5.u32, n * 16);
  return false;
}

// ----------------------------------------------------------------------------------------------------------
// Dessins
// ----------------------------------------------------------------------------------------------------------
NGPU_HOOK(925CCB68) {
  return DoDraw(base, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, nullptr, false);
}
// DrawIndexedVerticesUP(dev, prim, minVertex, nbVertices, nbIndices, pIndices, format (4 = 32 bits sinon 16), pSommets, stride [pile : r1+84])
NGPU_HOOK(925CD0E0) {
  const uint32_t prim = ctx.r4.u32, nidx = ctx.r7.u32, pidx = ctx.r8.u32, fmt = ctx.r9.u32, pv = ctx.r10.u32;
  const uint32_t stride = Be32(base + ctx.r1.u32 + 84);
  std::vector<uint32_t> idx(nidx);
  for (uint32_t i = 0; i < nidx; i++) idx[i] = (fmt & 4) ? Be32(base + pidx + 4 * i) : Be16(base + pidx + 2 * i);
  return DoDraw(base, ctx.r3.u32, prim, nidx, pv, stride, &idx, fmt & 4);
}
static struct { uint32_t prim = 0, count = 0, stride = 0, ptr = 0; } g_pending;
NGPU_HOOK(925CC6B8) {
  const uint32_t prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  NGPU_ORIG(925CC6B8);
  g_pending = {prim, count, stride, ctx.r3.u32};
  return true;
}
NGPU_HOOK(925CCB58) {
  if (g_pending.ptr) {
    auto p = g_pending;
    g_pending = {};
    DoDraw(base, ctx.r3.u32, p.prim, p.count, p.ptr, p.stride, nullptr, false);
  }
  return false;
}

// ----------------------------------------------------------------------------------------------------------
// Clear et Swap (appeles depuis hooks_frame.cpp)
// ----------------------------------------------------------------------------------------------------------
namespace ngpu {

void FrontClear(const uint8_t* base, uint32_t dev, uint32_t nrects, uint32_t rects, uint32_t flags, uint32_t color, double z, uint32_t stencil) {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (!EnsureCore()) return;
  BeginFrameIfNeeded();
  BuildShadow(base, dev);
  ng::ClearCall clear;
  clear.state.data = g.shadow;
  if (const char* di = std::getenv("DASH_NGPU_DRAWINFO"); di && g.frames == uint64_t(std::atoll(di))) {
    const xenos::DeviceState st{g.shadow};
    std::fprintf(stderr, "[clear] flags=%X surf=%08X ci=%08X di=%08X vp=%g,%g,%g,%g rects=%u\n", flags, st.Reg(0x2000), st.Reg(0x2001), st.Reg(0x2002), st.RegF(0x210F), st.RegF(0x2110), st.RegF(0x2111), st.RegF(0x2112), nrects);
  }
  clear.flags = flags & 0x3F;
  if (nrects && rects) {
    clear.has_rect = true;
    for (int i = 0; i < 4; ++i) clear.rect[i] = int32_t(Be32(base + rects + 4 * i));
  }
  clear.color[0] = ((color >> 16) & 255) / 255.f;
  clear.color[1] = ((color >> 8) & 255) / 255.f;
  clear.color[2] = (color & 255) / 255.f;
  clear.color[3] = ((color >> 24) & 255) / 255.f;
  clear.depth = float(z);
  clear.stencil = stencil;
  g.core->renderer().Clear(clear);
}

// Echange : copie la surface de couleur courante vers le tampon d'image fictif, soumet l'image et recupere celle
// terminee (le moteur a une image de retard) pour la presenter.
void FrontSwap(const uint8_t* base, uint32_t dev) {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (!EnsureCore()) return;
  ng::Renderer& renderer = g.core->renderer();
  BeginFrameIfNeeded();
  BuildShadow(base, dev);
  PutBe32(g.shadow + xenos::RegisterOffset(0x2300 + kCopyDestBase), kFrontBufferAddress);
  PutBe32(g.shadow + xenos::RegisterOffset(0x2300 + kCopyDestPitch), kBackbufferWidth | (kBackbufferHeight << 16));
  if (const char* di = std::getenv("DASH_NGPU_DRAWINFO"); di && g.frames + 1 == uint64_t(std::atoll(di))) {
    const xenos::DeviceState st{g.shadow};
    std::fprintf(stderr, "[swap %llu] surf=%08X ci=%08X di=%08X vp=%g,%g,%g,%g\n", (unsigned long long)g.frames, st.Reg(0x2000), st.Reg(0x2001), st.Reg(0x2002), st.RegF(0x210F), st.RegF(0x2110), st.RegF(0x2111), st.RegF(0x2112));
  }
  ng::ResolveCall resolve;
  resolve.state.data = g.shadow;
  renderer.Resolve(resolve);
  renderer.EndFrame(kFrontBufferAddress);
  g.in_frame = false;
  g.frames++;
  if (const char* dm = std::getenv("DASH_NGPU_DUMPRES")) {  // "<image>:<fichier.ppm>" : texture resolue (avant l'empaquetage) de cette image
    char* colon = nullptr;
    const unsigned long want = std::strtoul(dm, &colon, 10);
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    renderer.WaitIdle();
    if (colon && *colon == ':' && want == g.frames && renderer.ReadResolved(kFrontBufferAddress, rgba, w, h)) {
      if (FILE* f = std::fopen(colon + 1, "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < size_t(w) * h; ++i) {
          const uint8_t rgb[3] = {rgba[i * 4 + 2], rgba[i * 4 + 1], rgba[i * 4]};
          std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
      }
    }
  }
  if (const uint32_t* pixels = renderer.presented()) {
    if (const char* dm = std::getenv("DASH_NGPU_DUMP")) {  // "<image>:<fichier.ppm>" : image presentee par le noyau
      char* colon = nullptr;
      const unsigned long want = std::strtoul(dm, &colon, 10);
      if (colon && *colon == ':' && want == g.frames) {
        if (FILE* f = std::fopen(colon + 1, "wb")) {
          const uint32_t w = renderer.presented_width(), h = renderer.presented_height();
          std::fprintf(f, "P6\n%u %u\n255\n", w, h);
          for (size_t i = 0; i < size_t(w) * h; ++i) {
            const uint8_t rgb[3] = {uint8_t((pixels[i] & 1023) >> 2), uint8_t(((pixels[i] >> 10) & 1023) >> 2), uint8_t(((pixels[i] >> 20) & 1023) >> 2)};
            std::fwrite(rgb, 1, 3, f);
          }
          std::fclose(f);
        }
      }
    }
    VkPresentPacked(pixels, renderer.presented_width(), renderer.presented_height());
  } else {
    VkPresent();
  }
  if (g.frames == 1 || g.frames % 600 == 0) {
    for (const auto& [what, n] : renderer.stats()) NGPU_LOG("noyau : %s : %u", what.c_str(), n);
    if (std::getenv("DASH_NGPU_STATS"))
      for (const auto& [what, n] : renderer.stats()) std::fprintf(stderr, "[ngpu] image %llu : %s : %u\n", (unsigned long long)g.frames, what.c_str(), n);
  }
}

}  // namespace ngpu
