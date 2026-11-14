// SPDX-License-Identifier: GPL-3.0-or-later
// native_gpu : facade D3D du Dashboard 13604 (XDK 2.0.20158) sur le noyau de rendu commun ngpu_core. Aucun plugin GPU, aucune lecture
// de tampon de commandes : les fonctions D3D de la XDK liees dans dash.xex sont crochetees, le noyau rend l'etat Xenos que le D3D
// invite a deja ecrit dans son peripherique (docs/ngpu_core.md, section 13604).
//
// Origine : meme principe que front6770.cpp (Dashboard 6770) et que la facade generique games/_generic/native/front.cpp (reprise de la
// couche D3D native de Skate1Native, Kurtisone/Skate1Recompiled, GPL-3.0, commit 81805f51), avec les differences de la XDK 20158 :
//   - le D3D invite ecrit ses constantes et ses fetch constants DANS le peripherique (disposition canonique, sauf les objets shader
//     et le tampon d'indices decales de 184 octets) : l'ombre est un instantane du peripherique (xenos::Canonicalize), sans crochet
//     Set*ShaderConstantF ni SetTexture ;
//   - le D3D patche lui-meme les vfetch du microcode du vertex shader selon la declaration : la facade lit la partie physique de
//     l'objet APRES l'appel d'origine (comme la facade generique), sans table de declarations ;
//   - les dessins UP passent par BeginVertices (DrawVerticesUP en est une enveloppe) : ouverts au crochet, clos au prochain appel D3D
//     suivi (DrawVerticesUP fond EndVertices dans son corps) ; DrawIndexedVerticesUP est crochete a part.
// Le rendu est synchrone (comme 6770) : chaque appel est execute par le noyau dans le fil appelant, sous un verrou.
//
// Crochets : NGPU_HOOK(<adresse>) de ngpu.h, adresses de profiles/dash13604.h ; l'enveloppeur de d3d_trace.cpp les appelle.

#include <sys/stat.h>

#include <algorithm>
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
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/system/kernel_state.h>

#include "device.h"
#include "device_layout.h"
#include "ngpu.h"
#include "vk_backend.h"
#include "write_watcher.h"
#include NGPU_PROFILE_HEADER

namespace ngpu {
// Superposition de l'overlay du HUD de XAM (hud_front.cpp, palier N2) : symbole faible, absent des builds sans HUD recompile.
bool HudComposite(uint32_t* pixels, uint32_t w, uint32_t h) __attribute__((weak));
void NullGpuAttach(uint32_t dev);
void NullGpuCompleteNow(uint8_t* base, uint32_t dev);
void NextFrame();
}  // namespace ngpu

namespace {

using namespace ngpu;
namespace ng = native_gpu;

// ---- une instance, un verrou ----
struct ShaderObject {
  bool pixel = false;
  uint64_t key = 0;
  uint32_t virtual_size = 0, physical_size = 0, microcode_offset = 0;
  std::vector<std::pair<uint32_t, uint32_t>> vfetch;  // vertex shader : (adresse d'instruction, usage) des elements d'entree
};

struct InlineDraw {
  bool open = false;
  uint32_t vertices = 0, primitive = 0, count = 0, stride = 0, vs = 0, ps = 0;
  std::vector<uint8_t> state;      // ombre au moment de BeginVertices
  std::vector<uint8_t> vs_physical;  // microcode patche pour ce dessin
  float planes[24] = {};
};

struct Front {
  std::mutex mutex;
  bool init_tried = false;
  std::unique_ptr<PhysicalWriteWatcher> watcher;
  std::unique_ptr<ng::Core> core;
  bool in_frame = false;
  uint64_t frames = 0;
  uint32_t device = 0, candidate = 0;
  uint32_t front_buffer = 0;
  std::unordered_map<uint32_t, ShaderObject> objects;               // objet D3D -> infos (cache)
  std::unordered_map<uint64_t, std::vector<uint8_t>> containers;    // cle -> conteneur (virtuel + physique), copie
  std::map<std::string, int> missing;
  InlineDraw inl;
  alignas(16) uint8_t shadow[xenos::kCanonicalStateSize] = {};
  std::vector<uint8_t> scratch, scratch_idx, vs_copy;
  uint64_t draws = 0, clipped = 0, skipped = 0, clears = 0, resolves = 0, tiling = 0;
} g;

const xenos::DeviceLayout& Layout() {
  static const xenos::DeviceLayout layout = [] {
    xenos::DeviceLayout l;
    l.index_buffer = ngpu_profile::kIndexBuffer;
    l.pixel_shader = ngpu_profile::kPixelShader;
    l.vertex_shader = ngpu_profile::kVertexShader;
    return l;
  }();
  return layout;
}

bool Evlog() {
  static const bool on = std::getenv("DASH_NGPU_EVLOG") != nullptr;
  return on;
}

// Dossier des <cle>.spv : DASH_NGPU_SHADERS, sinon <game_data_root>/shaders/dash (fixé par dash_app_base.cpp ; relatif
// au dossier courant en repli, qui est le dossier de données sous l'AppImage).
std::string ShaderDir() {
  const char* e = std::getenv("DASH_NGPU_SHADERS");
  return e && *e ? e : "shaders/dash";
}
// Shader absent : un dossier absent donne un seul message (lancer la régénération) ; un dossier présent mais incomplet donne
// une ligne par shader.
void ReportMissingShader(uint64_t key, bool pixel, uint32_t object) {
  static bool dir_reported = false;
  const std::string dir = ShaderDir();
  struct stat st;
  if (::stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    if (!dir_reported) {
      dir_reported = true;
      NGPU_ERR("shaders natifs du Dashboard introuvables dans %s : lancer la régénération (XenonDash --regen-shaders)", dir.c_str());
    }
    return;
  }
  NGPU_ERR("SPIR-V introuvable pour le shader %s %016llX (objet %08X) dans %s", pixel ? "PS" : "VS", (unsigned long long)key, object, dir.c_str());
}

std::string MissingDir() {
  const char* e = std::getenv("DASH_NGPU_MISSING");
  return e && *e ? e : "";
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
  std::fprintf(stderr, "[ngpu] noyau ngpu_core sur '%s' (profil 13604)\n", core->device_name().c_str());
  // Rampe d'affichage : meme approximation que le Dashboard 6770 (puissance 1,16 ; DASH_NGPU_GAMMA=<p>, 1 = aucune), le D3D regle l'ecran
  // par une rampe PWL ecrite dans le tampon de commandes, non lu ici.
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

// ---- peripherique : capture a l'execution (r3 d'une methode du peripherique, confirmee par deux appels de suite) ----
bool PlausibleDevice(const uint8_t* base, uint32_t device) {
  if (device < 0x10000 || device >= 0xF0000000u || (device & 3)) return false;
  const uint32_t fence = Be32(base + device + ngpu_profile::kFencePtr);
  return fence == 0 || (fence >= 0x10000 && !(fence & 3));
}
bool IsDevice(const uint8_t* base, uint32_t device) {
  if (device == g.device) return true;
  if (PlausibleDevice(base, device) && device == g.candidate) {
    std::fprintf(stderr, "[ngpu] D3DDevice capture a %08X\n", device);
    g.device = device;
    return true;
  }
  g.candidate = device;
  return false;
}

// ---- ombre canonique : instantane du peripherique, adresses de textures de la region E corrigees (docs/rendu_natif.md §12.1) ----
void Snapshot(const uint8_t* base, uint8_t* out) {
  xenos::Canonicalize(Layout(), base + g.device, out);
  for (uint32_t slot = 0; slot < 32; ++slot) {
    uint8_t* fetch = out + xenos::device::kFetchConstants + 24 * slot;
    uint32_t d[6];
    for (int i = 0; i < 6; ++i) d[i] = Be32(fetch + 4 * i);
    if ((d[0] & 3) != 2) continue;  // pas une texture
    const auto fix = [](uint32_t word) {
      const uint32_t addr = word & 0xFFFFF000;
      return addr >= 0xE0000000u ? (word & 0xFFF) | (addr + 0x1000) : word;
    };
    d[1] = fix(d[1]);
    if (d[5] & 0xFFFFF000) d[5] = fix(d[5]);
    for (int i = 0; i < 6; ++i) PutBe32(fetch + 4 * i, d[i]);
  }
}

// Plans de coupe utilisateur (PA_CL_UCP) : le D3D les garde dans le peripherique a +10272 (6 plans de 4 float), hors des groupes de registres de l'ombre.
constexpr uint32_t kClipPlanes = 10272;
struct ClipPlanes {
  float v[24] = {};
};
ClipPlanes CapturePlanes(const uint8_t* base) {
  ClipPlanes p;
  for (int i = 0; i < 24; ++i) p.v[i] = BeF32(base + g.device + kClipPlanes + 4 * i);
  return p;
}

// ---- shaders ----
// Objet D3D (XDK 20158) : vertex shader = en-tete de 40 octets + 832 octets d'etat de vfetch, la partie virtuelle du conteneur copiee a
// +872, pointeur vers la copie de la partie physique a +32 ; pixel shader = copie virtuelle a +40, pointeur physique a +24.
constexpr uint32_t kVsVirtual = 872, kVsPhysicalPtr = ngpu_profile::kShaderPhysicalPtr, kPsVirtual = 40, kPsPhysicalPtr = 24;

// Enregistre l'objet depuis sa propre memoire (aucun crochet de creation necessaire). Renvoie nullptr si ce n'est pas un shader.
const ShaderObject* RegisterObject(const uint8_t* base, uint32_t object, bool pixel) {
  auto it = g.objects.find(object);
  if (it != g.objects.end() && it->second.pixel == pixel) return &it->second;
  const uint32_t virt = object + (pixel ? kPsVirtual : kVsVirtual);
  const uint32_t physical = Be32(base + object + (pixel ? kPsPhysicalPtr : kVsPhysicalPtr));
  const uint8_t* c = base + virt;
  if ((Be32(c) & 0xFFFFFFFE) != 0x102A1100 || !physical) return nullptr;
  if ((Be32(c) & 1) == uint32_t(pixel)) return nullptr;  // magie 0x102A1101 = vertex, 0x102A1100 = pixel
  ShaderObject info;
  info.pixel = pixel;
  info.virtual_size = Be32(c + 4);
  info.physical_size = Be32(c + 8);
  const uint8_t* shader = c + Be32(c + 0x18);
  info.microcode_offset = Be32(shader);
  const uint32_t size = Be32(shader + 4);
  if (!size || size > 0x40000 || info.virtual_size > 0x10000 || info.physical_size > 0x80000) return nullptr;
  info.key = XXH3_64bits(base + physical + info.microcode_offset, size);
  if (!pixel) {
    const uint32_t f18 = Be32(shader + 0x18), count = Be32(shader + 0x1C);
    for (uint32_t i = 0; i < count && i < 16; ++i) {
      const uint32_t v = Be32(shader + 0x24 + 4 * (f18 + i));
      info.vfetch.push_back({v & 0xFFF, (v >> 12) & 0xF});
    }
  }
  if (!g.containers.count(info.key)) {
    std::vector<uint8_t> container(info.virtual_size + info.physical_size);
    std::memcpy(container.data(), base + virt, info.virtual_size);
    std::memcpy(container.data() + info.virtual_size, base + physical, info.physical_size);
    char name[48];
    std::snprintf(name, sizeof name, "/%016llX.spv", (unsigned long long)info.key);
    const std::string path = ShaderDir() + name;
    if (EnsureCore()) {
      if (FileExists(path)) {
        g.core->shaders().AddFile(info.key, path);
      } else {
        const std::string dir = MissingDir();
        ReportMissingShader(info.key, pixel, object);
        if (!dir.empty()) {  // boucle « jouer -> reconstruire » : le conteneur est ecrit pour tools/dash_shaders.py
          std::snprintf(name, sizeof name, "/%016llX_%s.bin", (unsigned long long)info.key, pixel ? "PS" : "VS");
          if (FILE* f = std::fopen((dir + name).c_str(), "wb")) {
            std::fwrite(container.data(), 1, container.size(), f);
            std::fclose(f);
          }
        }
      }
    }
    g.containers.emplace(info.key, std::move(container));
  }
  return &(g.objects[object] = std::move(info));
}

// ---- elements de sommets : lus dans les vfetch du microcode patche (slot 0, format, decalage, pas) ----
struct Elem {
  uint32_t offset, format;
};
size_t ElementBytes(uint32_t format) {
  switch (format) {
    case 36: case 6: case 25: case 31: return 4;
    case 37: case 26: case 32: return 8;
    case 57: return 12;
    case 38: return 16;
    default: return 4;
  }
}
std::vector<Elem> Elements(const ShaderObject& vs, const uint8_t* physical, uint32_t physical_size) {
  std::vector<Elem> out;
  for (const auto& [address, usage] : vs.vfetch) {
    const uint32_t at = vs.microcode_offset + address * 12;
    if (at + 12 > physical_size) continue;
    const uint32_t w1 = Be32(physical + at + 4), w2 = Be32(physical + at + 8);
    out.push_back({((w2 >> 8) & 0x7FFFFF) * 4, (w1 >> 16) & 0x3F});
  }
  return out;
}

// Developpe rect list (8), fan (5) et quads indexes (13) en liste de triangles : le quatrieme sommet d'un rectangle = b + c - a.
void ExpandPrimitive(uint32_t prim, uint32_t count, const uint8_t* data, uint32_t stride, const uint8_t* indices, bool indices32,
                     const std::vector<Elem>& elems, std::vector<uint8_t>& out, uint32_t& out_count) {
  auto index = [&](uint32_t i) -> uint32_t {
    if (!indices) return i;
    return indices32 ? Be32(indices + 4 * size_t(i)) : Be16(indices + 2 * size_t(i));
  };
  auto vertex = [&](uint32_t i) { return data + size_t(index(i)) * stride; };
  out.clear();
  out_count = 0;
  auto emit = [&](const uint8_t* v) {
    out.insert(out.end(), v, v + stride);
    ++out_count;
  };
  std::vector<uint8_t> fourth(stride);
  auto combine = [&](const uint8_t* a, const uint8_t* b, const uint8_t* c) {
    std::memcpy(fourth.data(), c, stride);
    for (const Elem& e : elems) {
      if (e.offset + ElementBytes(e.format) > stride) continue;
      uint8_t* d = fourth.data() + e.offset;
      switch (e.format) {
        case 36: case 37: case 57: case 38: {
          const uint32_t n = e.format == 36 ? 1 : e.format == 37 ? 2 : e.format == 57 ? 3 : 4;
          for (uint32_t k = 0; k < n; ++k) {
            const float r = BeF32(b + e.offset + 4 * k) + BeF32(c + e.offset + 4 * k) - BeF32(a + e.offset + 4 * k);
            uint32_t bits;
            std::memcpy(&bits, &r, 4);
            PutBe32(d + 4 * k, bits);
          }
          break;
        }
        case 6:
          for (int k = 0; k < 4; ++k) d[k] = uint8_t(std::clamp(int(b[e.offset + k]) + c[e.offset + k] - a[e.offset + k], 0, 255));
          break;
        case 25: case 26: {
          const uint32_t n = e.format == 25 ? 2 : 4;
          for (uint32_t k = 0; k < n; ++k) {
            const int v = int(int16_t(Be16(b + e.offset + 2 * k))) + int16_t(Be16(c + e.offset + 2 * k)) - int16_t(Be16(a + e.offset + 2 * k));
            const uint16_t u = uint16_t(int16_t(std::clamp(v, -32768, 32767)));
            d[2 * k] = u >> 8;
            d[2 * k + 1] = u & 0xFF;
          }
          break;
        }
        default:
          std::memcpy(d, a + e.offset, ElementBytes(e.format));
          break;
      }
    }
    return fourth.data();
  };
  if (prim == 8) {
    for (uint32_t i = 0; i + 2 < count; i += 3) {
      const uint8_t *a = vertex(i), *b = vertex(i + 1), *c = vertex(i + 2);
      std::vector<uint8_t> dd(combine(a, b, c), combine(a, b, c) + stride);
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

// ---- dessin ----
struct DrawSource {
  uint32_t primitive = 0, count = 0, stride = 0;
  const uint8_t* vertices = nullptr;   // sommets en ligne (UP / BeginVertices) ou nullptr (flux du peripherique)
  uint32_t vertex_bytes = 0;
  const uint8_t* indices = nullptr;    // indices en ligne, grand-boutistes
  bool indices32 = false;
  uint32_t start = 0, base_vertex = 0, indices_address = 0, indices_size = 0;  // dessins par flux
};

// Appelee avec g.mutex tenu. `state` : ombre figee ; `vs_physical` : microcode patche pour ce dessin.
void ExecuteDraw(const uint8_t* base, uint32_t vs_obj, uint32_t ps_obj, const uint8_t* state, const uint8_t* vs_physical, uint32_t vs_physical_size,
                 const DrawSource& src, const float* planes) {
  ++g.draws;
  auto skip = [&](const char* why) {
    ++g.skipped;
    if (g.skipped < 20 || g.skipped % 1000 == 0) NGPU_ERR("dessin %llu ignore : %s (vs %08X ps %08X, %llu ignores)", (unsigned long long)g.draws, why, vs_obj, ps_obj, (unsigned long long)g.skipped);
  };
  if (std::getenv("DASH_NGPU_NODRAW") || !EnsureCore()) return;
  if (!vs_obj || !ps_obj) return skip("aucun shader lie");
  const ShaderObject* vs = RegisterObject(base, vs_obj, false);
  const ShaderObject* ps = RegisterObject(base, ps_obj, true);
  if (!vs || !ps) return skip("objet shader illisible");
  BeginFrameIfNeeded();
  ng::DrawCall call;
  call.state.data = state;
  {  // plans de coupe utilisateur (PA_CL_CLIP_CNTL 0x2204, bits 0-5) alignes sur les axes -> rectangle de coupe ; les autres sont ignores
    const xenos::DeviceState st{state};
    const uint32_t clip = st.Reg(0x2204);
    const uint32_t enabled = (clip & (1u << 16)) ? 0 : clip & 0x3F;
    if (enabled && planes) {
      float lo[2] = {-1e30f, -1e30f}, hi[2] = {1e30f, 1e30f};  // bornes en NDC (x, y)
      bool empty = false, unsupported = false;
      for (int i = 0; i < 6; ++i) {
        if (!(enabled >> i & 1)) continue;
        const float a = planes[4 * i], b = planes[4 * i + 1], c = planes[4 * i + 2], d = planes[4 * i + 3];
        if (c != 0.0f || (a != 0.0f && b != 0.0f)) { unsupported = true; continue; }
        if (a == 0.0f && b == 0.0f) { if (d < 0.0f) empty = true; continue; }   // plan constant : tout ou rien
        const int axis = a != 0.0f ? 0 : 1;
        const float k = axis ? b : a, bound = -d / k;                          // k * v + d >= 0
        if (k > 0.0f) lo[axis] = std::max(lo[axis], bound); else hi[axis] = std::min(hi[axis], bound);
      }
      if (unsupported) {
        static int warned = 0;
        if (warned++ < 3) NGPU_ERR("plan de coupe utilisateur non aligne sur les axes : ignore");
      }
      if (lo[0] > hi[0] || lo[1] > hi[1]) empty = true;
      if (empty) { ++g.clipped; return; }
      if (lo[0] > -1e29f || hi[0] < 1e29f || lo[1] > -1e29f || hi[1] < 1e29f) {
        const float xs = st.RegF(0x210F), xo = st.RegF(0x2110), ys = st.RegF(0x2111), yo = st.RegF(0x2112);
        float px0 = lo[0] > -1e29f ? xo + xs * lo[0] : -1e30f, px1 = hi[0] < 1e29f ? xo + xs * hi[0] : 1e30f;
        float py0 = lo[1] > -1e29f ? yo + ys * lo[1] : -1e30f, py1 = hi[1] < 1e29f ? yo + ys * hi[1] : 1e30f;
        if (ys < 0) {  // y inverse : NDC y bas -> pixel y haut
          py0 = hi[1] < 1e29f ? yo + ys * hi[1] : -1e30f;
          py1 = lo[1] > -1e29f ? yo + ys * lo[1] : 1e30f;
        }
        call.has_scissor = true;
        call.scissor[0] = int32_t(std::floor(std::max(px0, -1e6f)));
        call.scissor[1] = int32_t(std::floor(std::max(py0, -1e6f)));
        call.scissor[2] = int32_t(std::ceil(std::min(px1, 1e6f)));
        call.scissor[3] = int32_t(std::ceil(std::min(py1, 1e6f)));
        if (call.scissor[2] <= call.scissor[0] || call.scissor[3] <= call.scissor[1]) { ++g.clipped; return; }
      }
    }
  }
  call.vs_key = vs->key;
  call.ps_key = ps->key;
  call.vs_physical = vs_physical;
  call.vs_physical_size = vs_physical_size;
  uint32_t prim = src.primitive;
  if (src.vertices) {
    uint32_t max_index = src.count;
    if (src.indices) {
      max_index = 0;
      for (uint32_t i = 0; i < src.count; ++i) {
        const uint32_t v = src.indices32 ? Be32(src.indices + 4 * size_t(i)) : Be16(src.indices + 2 * size_t(i));
        max_index = std::max(max_index, v + 1);
      }
    }
    if (prim == 5 || prim == 8 || (prim == 13 && src.indices)) {
      uint32_t out_count = 0;
      ExpandPrimitive(prim, src.count, src.vertices, src.stride, src.indices, src.indices32, Elements(*vs, vs_physical, vs_physical_size), g.scratch, out_count);
      if (!out_count) return;
      call.primitive = 4;
      call.count = out_count;
      call.inline_vertices = g.scratch.data();
      call.inline_size = uint32_t(g.scratch.size());
    } else {
      call.primitive = prim;
      call.count = src.count;
      call.inline_vertices = src.vertices;
      call.inline_size = max_index * src.stride;
      if (src.indices) {
        call.inline_indices = src.indices;
        call.indices_size = src.count * (src.indices32 ? 4 : 2);
        call.indices_32bit = src.indices32;
      }
    }
  } else {
    call.primitive = prim;
    call.count = src.count;
    call.start = src.start;
    call.base_vertex = src.base_vertex;
    call.indices_address = src.indices_address;
    call.indices_size = src.indices_size;
    call.indices_32bit = src.indices32;
  }
  {  // DASH_NGPU_DRAWRANGE="a:b" : ne dessine que les dessins a..b de chaque image (bissection) ; DASH_NGPU_DRAWINFO=<image> : etat de chaque dessin
    static uint64_t frame = ~0ull, k = 0;
    if (frame != g.frames) { frame = g.frames; k = 0; }
    ++k;
    static const char* range = std::getenv("DASH_NGPU_DRAWRANGE");
    if (range) {
      unsigned a = 0, b = ~0u;
      std::sscanf(range, "%u:%u", &a, &b);
      if (k < a || k > b) return;
    }
    if (const char* rv = std::getenv("DASH_NGPU_REGVARY"); rv && g.frames == uint64_t(std::atoll(rv))) {  // registres qui varient d'un dessin a l'autre
      static uint8_t last[xenos::kCanonicalStateSize];
      static bool have = false;
      static std::map<uint32_t, int> changed;
      if (have)
        for (uint32_t o = 10368; o < 10960; o += 4)
          if (std::memcmp(last + o, state + o, 4) && changed[o]++ == 0)
            std::fprintf(stderr, "[regvary] dessin %llu : +%u %08X -> %08X\n", (unsigned long long)k, o, Be32(last + o), Be32(state + o));
      std::memcpy(last, state, xenos::kCanonicalStateSize);
      have = true;
    }
    static const char* skip_range = std::getenv("DASH_NGPU_DRAWSKIP");
    if (skip_range) {  // "a:b" : saute les dessins a..b de chaque image
      unsigned a = 0, b = 0;
      std::sscanf(skip_range, "%u:%u", &a, &b);
      if (k >= a && k <= b) return;
    }
    if (const char* rd = std::getenv("DASH_NGPU_REGDUMP"); rd && g.frames == uint64_t(std::atoll(rd)) && (k == 46 || k == 48)) {  // image fixe, dessins 46 et 48
      for (uint32_t o = 10368; o < 10960; o += 4) std::fprintf(stderr, "[regdump %llu] +%u %08X %g\n", (unsigned long long)k, o, Be32(state + o), BeF32(state + o));
    }
    if (const char* di = std::getenv("DASH_NGPU_DRAWINFO"); di && g.frames == uint64_t(std::atoll(di))) {
      const xenos::DeviceState st{state};
      std::fprintf(stderr, "[di %llu] prim=%u n=%u stride=%u vs=%016llX ps=%016llX blend=%08X cc=%08X dc=%08X mode=%08X vp=%g,%g,%g,%g r200E=%08X r200F=%08X r2011=%08X r2012=%08X r2080=%08X\n",
                   (unsigned long long)k, prim, src.count, src.stride, (unsigned long long)vs->key, (unsigned long long)ps->key, st.Reg(0x2201), st.Reg(0x2202), st.Reg(0x2200),
                   st.Reg(0x2205), st.RegF(0x210F), st.RegF(0x2110), st.RegF(0x2111), st.RegF(0x2112), st.Reg(0x200E), st.Reg(0x200F), st.Reg(0x2011), st.Reg(0x2012), st.Reg(0x2080));
      std::fprintf(stderr, "     vsc0:");
      for (int c = 0; c < 8; ++c) std::fprintf(stderr, " %g", BeF32(state + xenos::device::kVertexConstants + 4 * c));
      std::fprintf(stderr, " | psc0:");
      for (int c = 0; c < 8; ++c) std::fprintf(stderr, " %g", BeF32(state + xenos::device::kPixelConstants + 4 * c));
      std::fprintf(stderr, " | planes(ena %X):", st.Reg(0x2204) & 0x3F);
      for (int c = 0; c < 16; ++c) std::fprintf(stderr, " %g", planes[c]);
      std::fprintf(stderr, " | v0:");
      for (int c = 0; c < 4 && src.vertices; ++c) std::fprintf(stderr, " %g", BeF32(src.vertices + 4 * c));
      std::fputc('\n', stderr);
    }
  }
  if (Evlog() && g.draws <= 40) {
    std::fprintf(stderr, "[ev] draw %llu prim=%u count=%u stride=%u vs=%016llX ps=%016llX inline=%d indexed=%d\n", (unsigned long long)g.draws, prim, src.count,
                 src.stride, (unsigned long long)vs->key, (unsigned long long)ps->key, src.vertices != nullptr, src.indices != nullptr || src.indices_address != 0);
  }
  g.core->renderer().Draw(call);
}

// Cloture le dessin ouvert par BeginVertices (appelee avec g.mutex tenu).
void CloseInline(const uint8_t* base) {
  if (!g.inl.open) return;
  g.inl.open = false;
  if (!g.inl.vertices || !g.inl.count || !g.inl.stride) return;
  DrawSource src;
  src.primitive = g.inl.primitive;
  src.count = g.inl.count;
  src.stride = g.inl.stride;
  src.vertices = base + g.inl.vertices;
  src.vertex_bytes = g.inl.count * g.inl.stride;
  ExecuteDraw(base, g.inl.vs, g.inl.ps, g.inl.state.data(), g.inl.vs_physical.data(), uint32_t(g.inl.vs_physical.size()), src, g.inl.planes);
}

// Copie de la partie physique de l'objet vertex shader tel que D3D l'a patche.
bool CopyVsPhysical(const uint8_t* base, uint32_t vs_obj, std::vector<uint8_t>& out) {
  if (!vs_obj) return false;
  const ShaderObject* vs = RegisterObject(base, vs_obj, false);
  if (!vs) return false;
  const uint32_t physical = Be32(base + vs_obj + kVsPhysicalPtr);
  out.assign(base + physical, base + physical + vs->physical_size);
  return true;
}

}  // namespace

// ----------------------------------------------------------------------------------------------------------
// Cycle de vie du GPU nul : attentes de la XDK D3D
// ----------------------------------------------------------------------------------------------------------
NGPU_HOOK(NGP_WaitFence) {
  ngpu::NullGpuCompleteNow(base, ctx.r3.u32);
  return false;
}
// (dev, pos, taille) : GPU draine = pointeur de lecture (mot +60 du bloc de retour) a la position demandee.
NGPU_HOOK(NGP_WaitDrain) {
  const uint32_t dev = ctx.r3.u32;
  ngpu::NullGpuCompleteNow(base, dev);
  const uint32_t idp = ngpu::Be32(base + dev + ngpu_profile::kFencePtr);
  if (idp) ngpu::PutBe32(base + idp + ngpu_profile::kDrainWord, ctx.r4.u32);
  return false;
}
// (dev, a, b) : second mot du bloc de retour (+4), ecrit par le GPU ; condition de sortie ((b - mot) & 3) == 0.
NGPU_HOOK(NGP_WaitSecond) {
  const uint32_t dev = ctx.r3.u32;
  ngpu::NullGpuCompleteNow(base, dev);
  const uint32_t idp = ngpu::Be32(base + dev + ngpu_profile::kFencePtr);
  if (idp) ngpu::PutBe32(base + idp + ngpu_profile::kSecondWord, ctx.r5.u32);
  return false;
}

// ----------------------------------------------------------------------------------------------------------
// Shaders (l'objet est aussi lu paresseusement au premier dessin : ces crochets ne font qu'avancer l'enregistrement)
// ----------------------------------------------------------------------------------------------------------
NGPU_HOOK(NGP_CreateVertexShader) {
  NGPU_ORIG(NGP_CreateVertexShader);
  const uint32_t object = ctx.r3.u32;
  if (object) {
    std::lock_guard<std::mutex> lock(g.mutex);
    RegisterObject(base, object, false);
  }
  return true;
}
NGPU_HOOK(NGP_CreatePixelShader) {
  NGPU_ORIG(NGP_CreatePixelShader);
  const uint32_t object = ctx.r3.u32;
  if (object) {
    std::lock_guard<std::mutex> lock(g.mutex);
    RegisterObject(base, object, true);
  }
  return true;
}

// ----------------------------------------------------------------------------------------------------------
// Dessins
// ----------------------------------------------------------------------------------------------------------
// (dev, prim, count, stride) -> pointeur ou le jeu ecrit les sommets : le dessin est clos au prochain appel D3D suivi.
NGPU_HOOK(NGP_BeginVertices) {
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, count = ctx.r5.u32, stride = ctx.r6.u32;
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    CloseInline(base);
  }
  NGPU_ORIG(NGP_BeginVertices);
  std::lock_guard<std::mutex> lock(g.mutex);
  if (!IsDevice(base, dev) || !ctx.r3.u32) return true;
  g.inl.open = true;
  g.inl.vertices = ctx.r3.u32;
  g.inl.primitive = prim;
  g.inl.count = count;
  g.inl.stride = stride;
  g.inl.vs = ngpu::Be32(base + dev + ngpu_profile::kVertexShader);
  g.inl.ps = ngpu::Be32(base + dev + ngpu_profile::kPixelShader);
  g.inl.state.resize(xenos::kCanonicalStateSize);
  Snapshot(base, g.inl.state.data());
  std::memcpy(g.inl.planes, CapturePlanes(base).v, sizeof g.inl.planes);
  if (!CopyVsPhysical(base, g.inl.vs, g.inl.vs_physical)) g.inl.vs_physical.clear();
  return true;
}
NGPU_HOOK(NGP_EndVertices) {
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    CloseInline(base);
  }
  return false;
}
// DrawIndexedVerticesUP(dev, prim, minVertex, nbSommets, nbIndices, indices, format (4 = 32 bits sinon 16), sommets, stride [pile : r1+84]).
NGPU_HOOK(NGP_DrawIndexedVerticesUP) {
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, nidx = ctx.r7.u32, pidx = ctx.r8.u32, fmt = ctx.r9.u32, pv = ctx.r10.u32;
  const uint32_t stride = ngpu::Be32(base + ctx.r1.u32 + 84);
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    CloseInline(base);
  }
  NGPU_ORIG(NGP_DrawIndexedVerticesUP);
  std::lock_guard<std::mutex> lock(g.mutex);
  if (!IsDevice(base, dev) || !g.core && !EnsureCore()) return true;
  if (!nidx || !stride || stride > 1020) return true;
  const uint32_t vs = ngpu::Be32(base + dev + ngpu_profile::kVertexShader), ps = ngpu::Be32(base + dev + ngpu_profile::kPixelShader);
  if (!CopyVsPhysical(base, vs, g.vs_copy)) return true;
  Snapshot(base, g.shadow);
  DrawSource src;
  src.primitive = prim;
  src.count = nidx;
  src.stride = stride;
  src.vertices = base + pv;
  src.indices = base + pidx;
  src.indices32 = fmt & 4;
  ExecuteDraw(base, vs, ps, g.shadow, g.vs_copy.data(), uint32_t(g.vs_copy.size()), src, CapturePlanes(base).v);
  return true;
}
// DrawVertices(dev, prim, premier sommet, nombre) et DrawIndexedVertices(dev, prim, sommet de base, premier indice, nombre) : flux du peripherique.
static void StreamDraw(uint8_t* base, bool indexed, uint32_t dev, uint32_t prim, uint32_t a, uint32_t b, uint32_t n) {
  std::lock_guard<std::mutex> lock(g.mutex);
  if (!IsDevice(base, dev) || !EnsureCore()) return;
  CloseInline(base);
  const uint32_t vs = ngpu::Be32(base + dev + ngpu_profile::kVertexShader), ps = ngpu::Be32(base + dev + ngpu_profile::kPixelShader);
  if (!CopyVsPhysical(base, vs, g.vs_copy)) return;
  Snapshot(base, g.shadow);
  DrawSource src;
  src.primitive = prim;
  src.count = n;
  if (indexed) {
    const uint32_t ib = ngpu::Be32(base + dev + ngpu_profile::kIndexBuffer);
    if (!ib) return;
    src.base_vertex = a;
    src.start = b;
    src.indices32 = ngpu::Be32(base + ib) >> 31;
    src.indices_address = rex::system::kernel_memory()->GetPhysicalAddress(ngpu::Be32(base + ib + 24));
    src.indices_size = ngpu::Be32(base + ib + 28);
  } else {
    src.start = a;
  }
  ExecuteDraw(base, vs, ps, g.shadow, g.vs_copy.data(), uint32_t(g.vs_copy.size()), src, CapturePlanes(base).v);
}
NGPU_HOOK(NGP_DrawVertices) {
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, a = ctx.r5.u32, n = ctx.r6.u32;
  NGPU_ORIG(NGP_DrawVertices);
  StreamDraw(base, false, dev, prim, a, 0, n);
  return true;
}
NGPU_HOOK(NGP_DrawIndexedVertices) {
  const uint32_t dev = ctx.r3.u32, prim = ctx.r4.u32, a = ctx.r5.u32, b = ctx.r6.u32, n = ctx.r7.u32;
  NGPU_ORIG(NGP_DrawIndexedVertices);
  StreamDraw(base, true, dev, prim, a, b, n);
  return true;
}

// ----------------------------------------------------------------------------------------------------------
// Clear, Resolve, Swap
// ----------------------------------------------------------------------------------------------------------
// ClearF(dev, drapeaux, rect*, couleur* (4 float grand-boutistes), f1 = z, r8 = stencil)
NGPU_HOOK(NGP_ClearF) {
  const uint32_t dev = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, color = ctx.r6.u32, stencil = ctx.r8.u32;
  const double z = ctx.f1.f64;
  ngpu::NullGpuAttach(dev);
  NGPU_ORIG(NGP_ClearF);
  std::lock_guard<std::mutex> lock(g.mutex);
  if (!IsDevice(base, dev) || !EnsureCore()) return true;
  CloseInline(base);
  BeginFrameIfNeeded();
  Snapshot(base, g.shadow);
  ng::ClearCall clear;
  clear.state.data = g.shadow;
  clear.flags = flags & 0x3F;
  if (rect) {
    clear.has_rect = true;
    for (int i = 0; i < 4; ++i) clear.rect[i] = int32_t(ngpu::Be32(base + rect + 4 * i));
  }
  for (int c = 0; c < 4 && color; ++c) clear.color[c] = ngpu::BeF32(base + color + 4 * c);
  clear.depth = float(z);
  clear.stencil = stencil;
  ++g.clears;
  if (Evlog() && g.clears <= 20)
    std::fprintf(stderr, "[ev] clear flags=%X rect=%d,%d,%d,%d color=%g,%g,%g,%g z=%g\n", flags, clear.rect[0], clear.rect[1], clear.rect[2], clear.rect[3],
                 clear.color[0], clear.color[1], clear.color[2], clear.color[3], z);
  g.core->renderer().Clear(clear);
  return true;
}
// Resolve public (dev, drapeaux, rect source*, texture, point destination*, ..., f1 = z de remise a zero) : enveloppe de 921A57B8.
NGPU_HOOK(NGP_Resolve) {
  const uint32_t dev = ctx.r3.u32, flags = ctx.r4.u32, rect = ctx.r5.u32, point = ctx.r7.u32;
  const double z = ctx.f1.f64;
  ngpu::NullGpuAttach(dev);
  NGPU_ORIG(NGP_Resolve);
  std::lock_guard<std::mutex> lock(g.mutex);
  if (!IsDevice(base, dev) || !EnsureCore()) return true;
  CloseInline(base);
  BeginFrameIfNeeded();
  Snapshot(base, g.shadow);
  ng::ResolveCall resolve;
  resolve.state.data = g.shadow;
  resolve.flags = flags;
  if (rect) {
    resolve.has_rect = true;
    for (int i = 0; i < 4; ++i) resolve.rect[i] = int32_t(ngpu::Be32(base + rect + 4 * i));
  }
  if (point) {
    resolve.has_dest_point = true;
    resolve.dest_x = int32_t(ngpu::Be32(base + point));
    resolve.dest_y = int32_t(ngpu::Be32(base + point + 4));
  }
  resolve.clear_depth = float(z);
  ++g.resolves;
  const uint32_t dest = xenos::DeviceState{g.shadow}.Reg(0x2319);  // RB_COPY_DEST_BASE
  if (!(flags & 4)) g.front_buffer = dest;
  if (Evlog() && g.resolves <= 20)
    std::fprintf(stderr, "[ev] resolve flags=%X rect=%d,%d,%d,%d point=%d,%d dest=%08X pitch=%08X\n", flags, resolve.rect[0], resolve.rect[1], resolve.rect[2],
                 resolve.rect[3], resolve.dest_x, resolve.dest_y, dest, xenos::DeviceState{g.shadow}.Reg(0x231A));
  g.core->renderer().Resolve(resolve);
  return true;
}
// BeginTiling : non suivi (rendu sans tuiles) ; l'effacement qu'il emet passe par ClearF.
NGPU_HOOK(NGP_BeginTiling) {
  ++g.tiling;
  if (Evlog() && g.tiling <= 5) std::fprintf(stderr, "[ev] BeginTiling flags=%X count=%u\n", ctx.r4.u32, ctx.r5.u32);
  return false;
}

// Swap : soumet l'image, recupere celle terminee (le noyau a une image de retard) et la presente.
NGPU_HOOK(NGP_Swap) {
  const uint32_t dev = ctx.r3.u32;
  ngpu::NullGpuAttach(dev);
  {
    std::lock_guard<std::mutex> lock(g.mutex);
    if (IsDevice(base, dev) && EnsureCore()) {
      CloseInline(base);
      ng::Renderer& renderer = g.core->renderer();
      BeginFrameIfNeeded();
      if (Evlog() && g.frames < 5) std::fprintf(stderr, "[ev] swap %llu front=%08X draws=%llu clears=%llu resolves=%llu skipped=%llu\n", (unsigned long long)g.frames,
                                                g.front_buffer, (unsigned long long)g.draws, (unsigned long long)g.clears, (unsigned long long)g.resolves, (unsigned long long)g.skipped);
      renderer.EndFrame(g.front_buffer);
      g.in_frame = false;
      g.frames++;
      if (const uint32_t* pixels = renderer.presented()) {
        static std::vector<uint32_t> composed;
        if (ngpu::HudComposite) {  // le HUD de XAM (boite de message...) est superpose a l'image du Dashboard
          composed.assign(pixels, pixels + size_t(renderer.presented_width()) * renderer.presented_height());
          if (ngpu::HudComposite(composed.data(), renderer.presented_width(), renderer.presented_height())) pixels = composed.data();
        }
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
  }
  NGPU_ORIG(NGP_Swap);
  ngpu::NextFrame();
  return true;
}

namespace ngpu {
static std::atomic<uint64_t> g_frame{0};
uint64_t FrameCounter() { return g_frame.load(); }
void NextFrame() { g_frame++; }
}  // namespace ngpu
